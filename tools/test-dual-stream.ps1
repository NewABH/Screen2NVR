param([switch]$Software, [switch]$Desktop, [switch]$Live, [switch]$Digest,
      [switch]$StreamBaseDigest, [switch]$LegacyDigest, [switch]$ContentBaseDigest)
$ErrorActionPreference = 'Stop'
if ($StreamBaseDigest -or $LegacyDigest -or $ContentBaseDigest) { $Digest = $true }
function Assert([bool]$condition, [string]$message) {
    if (!$condition) { throw $message }; Write-Host "PASS: $message"
}
Add-Type @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Threading.Tasks;
public class RtpStats {
    public long Ssrc=-1, First=-1, Last=-1;
    public int Frames=0, Packets=0;
    public int IdrFrames=0, PredictedFrames=0;
    public double WallSeconds=0;
    public string FirstIdrNalOrder="";
    public bool Idr=false;
}
public class RtpReader {
    static byte[] Read(Stream stream, int length) {
        byte[] bytes=new byte[length]; int offset=0;
        while(offset<length) { int n=stream.Read(bytes,offset,length-offset); if(n==0) throw new Exception("RTP EOF"); offset+=n; }
        return bytes;
    }
    static long U32(byte[] b,int p) { return ((long)b[p]<<24)|((long)b[p+1]<<16)|((long)b[p+2]<<8)|b[p+3]; }
    public static Task<RtpStats> Receive(TcpClient client,int frames) {
        return Task.Run(()=> {
            var result=new RtpStats(); int previous=-1; var stream=client.GetStream();
            var clock=Stopwatch.StartNew(); double firstWall=-1; bool idrFrame=false, predictedFrame=false;
            var nalOrder=new List<int>(); long frameTimestamp=-1;
            while(result.Frames<frames) {
                var h=Read(stream,4); if(h[0]!=36 || h[1]!=0) throw new Exception("Bad interleaved channel");
                var p=Read(stream,(h[2]<<8)|h[3]); if(p.Length<14 || (p[0]>>6)!=2) throw new Exception("Bad RTP");
                int seq=(p[2]<<8)|p[3]; if(previous>=0 && seq!=((previous+1)&65535)) throw new Exception("RTP sequence gap/mixed streams"); previous=seq;
                long ssrc=U32(p,8); if(result.Ssrc>=0 && ssrc!=result.Ssrc) throw new Exception("SSRC changed"); result.Ssrc=ssrc;
                result.Packets++; int type=p[12]&31;
                long packetTimestamp=U32(p,4);
                if(frameTimestamp>=0 && packetTimestamp!=frameTimestamp) throw new Exception("Timestamp changes inside an access unit");
                frameTimestamp=packetTimestamp;
                int nalType=type==28 ? p[13]&31 : type;
                if(type!=28 || (p[13]&128)!=0) nalOrder.Add(nalType);
                if(nalType==5) { result.Idr=true; idrFrame=true; }
                if(nalType==1) predictedFrame=true;
                if((p[1]&128)!=0) {
                    long t=U32(p,4); if(t<=result.Last) throw new Exception("RTP timestamp order");
                    if(result.First<0) { result.First=t; firstWall=clock.Elapsed.TotalSeconds; }
                    result.Last=t; result.Frames++;
                    if(idrFrame) {
                        int aud=nalOrder.IndexOf(9);
                        if(aud>0) throw new Exception("AUD is not the first NAL in the access unit");
                        int sps=nalOrder.IndexOf(7), pps=nalOrder.IndexOf(8), idr=nalOrder.IndexOf(5);
                        if(sps<0 || pps<=sps || idr<=pps) throw new Exception("IDR parameter sets missing/out of order");
                        if(nalOrder.FindAll(n=>n==7).Count!=1 || nalOrder.FindAll(n=>n==8).Count!=1) throw new Exception("Duplicated IDR SPS/PPS");
                        result.IdrFrames++; if(result.FirstIdrNalOrder.Length==0) result.FirstIdrNalOrder=String.Join(",",nalOrder);
                    }
                    else if(predictedFrame) result.PredictedFrames++;
                    nalOrder.Clear(); frameTimestamp=-1;
                    idrFrame=false; predictedFrame=false;
                    result.WallSeconds=clock.Elapsed.TotalSeconds-firstWall;
                }
            }
            return result;
        });
    }
}
public class BaselineSps {
    public int Width, Height, Profile, Level;
    public double VuiFps;
    public bool FixedFrameRate;
    byte[] data; int bit;
    uint Bits(int n) { uint v=0; for(int i=0;i<n;i++) { if(bit>=data.Length*8) throw new Exception("Short SPS"); v=(v<<1)|(uint)((data[bit/8]>>(7-bit%8))&1); bit++; } return v; }
    uint Ue() { int zeros=0; while(Bits(1)==0) { if(++zeros>30) throw new Exception("Invalid SPS"); } return ((1u<<zeros)-1)+Bits(zeros); }
    int Se() { uint n=Ue(); return (n%2==0) ? -(int)(n/2) : (int)((n+1)/2); }
    public static BaselineSps Parse(byte[] nal) {
        var bytes=new List<byte>(); int zeros=0;
        for(int i=1;i<nal.Length;i++) { if(zeros==2 && nal[i]==3) { zeros=0; continue; } bytes.Add(nal[i]); zeros=nal[i]==0 ? zeros+1 : 0; }
        var r=new BaselineSps {data=bytes.ToArray()};
        uint profile=r.Bits(8); r.Profile=(int)profile; r.Bits(8); r.Level=(int)r.Bits(8); r.Ue();
        if(profile!=66 && profile!=77) throw new Exception("Expected Baseline/Main H264");
        r.Ue(); uint poc=r.Ue();
        if(poc==0) r.Ue(); else if(poc==1) { r.Bits(1); r.Se(); r.Se(); uint n=r.Ue(); for(uint i=0;i<n;i++) r.Se(); }
        r.Ue(); r.Bits(1); int w=(int)(r.Ue()+1)*16; int h=(int)(r.Ue()+1)*16;
        int frame=(int)r.Bits(1); if(frame==0) r.Bits(1); h*=2-frame; r.Bits(1);
        if(r.Bits(1)!=0) { int left=(int)r.Ue(), right=(int)r.Ue(), top=(int)r.Ue(), bottom=(int)r.Ue(); w-=2*(left+right); h-=2*(2-frame)*(top+bottom); }
        r.Width=w; r.Height=h;
        if(r.Bits(1)!=0) { // vui_parameters_present_flag
            if(r.Bits(1)!=0 && r.Bits(8)==255) { r.Bits(16); r.Bits(16); }
            if(r.Bits(1)!=0) r.Bits(1);
            if(r.Bits(1)!=0) { r.Bits(3); r.Bits(1); if(r.Bits(1)!=0) { r.Bits(8); r.Bits(8); r.Bits(8); } }
            if(r.Bits(1)!=0) { r.Ue(); r.Ue(); }
            if(r.Bits(1)!=0) { uint units=r.Bits(32), scale=r.Bits(32); r.FixedFrameRate=r.Bits(1)!=0; if(units!=0) r.VuiFps=scale/(2.0*units); }
        }
        return r;
    }
}
'@
function ReadExact($stream, [int]$size) {
    $buffer = New-Object byte[] $size; $offset = 0
    while ($offset -lt $size) { $n=$stream.Read($buffer,$offset,$size-$offset); if (!$n) { throw 'Unexpected EOF' }; $offset += $n }
    return ,$buffer
}
function Response($stream) {
    $header = ''
    while (!$header.EndsWith("`r`n`r`n")) {
        $b=$stream.ReadByte(); if ($b -lt 0 -or $header.Length -gt 16384) { throw 'Invalid response' }; $header += [char]$b
    }
    $body=''
    if ($header -match '(?i)Content-Length: (\d+)') { $body=[Text.Encoding]::UTF8.GetString((ReadExact $stream ([int]$Matches[1]))) }
    return @{Header=$header; Body=$body}
}
$basic = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('screen2nvr-test:temporary-test-password'))
$script:httpChallenge = ''; $script:httpCount = 0
$script:rtspChallenge = ''; $script:rtspCount = 0
function Md5Hex([string]$text) {
    $hash = [Security.Cryptography.MD5]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose() }
}
function DigestHeader([string]$challenge, [string]$method, [string]$uri, [int]$count) {
    if ($challenge -notmatch 'nonce="([^"]+)"') { throw 'No Digest challenge' }; $nonce = $Matches[1]
    $nc = $count.ToString('x8'); $cnonce = 'dual-stream-client'
    $ha1 = Md5Hex 'screen2nvr-test:Screen2NVR:temporary-test-password'
    if ($StreamBaseDigest -and $method -eq 'SETUP') { $uri = $uri.Substring(0, $uri.Length - 'trackID=0'.Length) }
    $ha2 = Md5Hex "${method}:$uri"
    if ($LegacyDigest -and $method -ne 'POST') {
        # A legacy NVR must be offered a compatible challenge, not just have its
        # no-qop response accepted despite the server advertising another mode.
        if ($challenge -notmatch 'algorithm=MD5' -or $challenge -match '(?i)\bqop\s*=') {
            throw 'Legacy NVR cannot negotiate this RTSP Digest challenge'
        }
        $response = Md5Hex "${ha1}:${nonce}:$ha2"
        return "Digest username=`"screen2nvr-test`", realm=`"Screen2NVR`", nonce=`"$nonce`", uri=`"$uri`", response=`"$response`""
    }
    $response = Md5Hex "${ha1}:${nonce}:${nc}:${cnonce}:auth:$ha2"
    return "Digest username=`"screen2nvr-test`", realm=`"Screen2NVR`", nonce=`"$nonce`", uri=`"$uri`", algorithm=MD5, qop=auth, nc=$nc, cnonce=`"$cnonce`", response=`"$response`""
}
function Onvif([string]$operation, [string]$parameters = '') {
    $body = '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:m="http://www.onvif.org/ver10/media/wsdl"><s:Body><m:'+$operation+'>'+$parameters+'</m:'+$operation+'></s:Body></s:Envelope>'
    for ($attempt=0; $attempt -lt 2; $attempt++) {
        $authorization = "Authorization: Basic $basic`r`n"
        if ($Digest) {
            $authorization = ''
            if ($script:httpChallenge) {
                $script:httpCount++
                $authorization = 'Authorization: ' + (DigestHeader $script:httpChallenge 'POST' '/onvif/device_service' $script:httpCount) + "`r`n"
            }
        }
        $client=[Net.Sockets.TcpClient]::new('127.0.0.1',18002)
        try {
            $client.ReceiveTimeout=3000; $stream=$client.GetStream()
            $request="POST /onvif/device_service HTTP/1.1`r`nHost: localhost`r`nContent-Type: application/soap+xml`r`n${authorization}Content-Length: $([Text.Encoding]::UTF8.GetByteCount($body))`r`n`r`n$body"
            $bytes=[Text.Encoding]::UTF8.GetBytes($request); $stream.Write($bytes,0,$bytes.Length)
            $response=Response $stream
            if ($Digest -and $response.Header -match '^HTTP/1.1 401' -and $attempt -eq 0) {
                $script:httpChallenge=$response.Header; $script:httpCount=0; continue
            }
            if ($response.Header -notmatch '^HTTP/1.1 200') { throw "ONVIF $operation failed: $($response.Header)" }
            return [xml]$response.Body
        } finally { $client.Dispose() }
    }
}
function Node($xml,[string]$name) { $xml.SelectSingleNode("//*[local-name()='$name']") }
function Rtsp($client,[string]$method,[string]$uri,[int]$sequence,[string]$headers='', [string]$credential=$basic) {
    $stream=$client.GetStream()
    $signedUri = $uri
    if ($ContentBaseDigest -and $method -in @('DESCRIBE','PLAY','GET_PARAMETER','TEARDOWN')) {
        # Request the SDP Content-Base alias but sign the original ONVIF stream URL.
        $signedUri = $uri.TrimEnd('/')
        $uri = $signedUri + '/'
    }
    $authorization=if ($credential) { "Authorization: Basic $credential`r`n" } else { '' }
    if ($Digest -and $credential -eq $basic) {
        if (!$script:rtspChallenge) {
            $probe=[Net.Sockets.TcpClient]::new('127.0.0.1',18556)
            try {
                $probe.ReceiveTimeout=3000
                $challenge=Rtsp $probe 'DESCRIBE' $uri 1 '' ''
                Assert ($challenge.Header -match '^RTSP/1.0 401') 'RTSP initial Digest challenge'
                $script:rtspChallenge=$challenge.Header
            } finally { $probe.Dispose() }
        }
        $script:rtspCount++
        $authorization='Authorization: ' + (DigestHeader $script:rtspChallenge $method $signedUri $script:rtspCount) + "`r`n"
    }
    $bytes=[Text.Encoding]::ASCII.GetBytes("$method $uri RTSP/1.0`r`nCSeq: $sequence`r`n$authorization$headers`r`n")
    $stream.Write($bytes,0,$bytes.Length); return Response $stream
}
function CheckRouting([string]$mainUri,[string]$subUri) {
    $client=[Net.Sockets.TcpClient]::new('127.0.0.1',18556)
    try {
        $client.ReceiveTimeout=3000
        $response=Rtsp $client 'DESCRIBE' $subUri 1 '' ''
        Assert ($response.Header -match '^RTSP/1.0 401') 'Secondary stream requires authentication'
        $response=Rtsp $client 'DESCRIBE' $subUri 2 '' 'aW52YWxpZDppbnZhbGlk'
        Assert ($response.Header -match '^RTSP/1.0 401') 'Secondary stream rejects wrong credentials'
        $response=Rtsp $client 'DESCRIBE' ($subUri+'-missing') 3
        Assert ($response.Header -match '^RTSP/1.0 404') 'Unknown path does not leak either stream'
        $response=Rtsp $client 'SETUP' "$mainUri/trackID=0" 4 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n"
        Assert ($response.Header -match '^RTSP/1.0 200') 'Routing check establishes main session'
        $response=Rtsp $client 'PLAY' $subUri 5
        Assert ($response.Header -match '^RTSP/1.0 455') 'Established session cannot switch to the other stream'
    } finally { $client.Dispose() }
}
function CheckUdp([string]$uri) {
    $client=[Net.Sockets.TcpClient]::new('127.0.0.1',18556)
    $udp=[Net.Sockets.UdpClient]::new(0)
    try {
        $client.ReceiveTimeout=5000; $udp.Client.ReceiveTimeout=5000
        $port=$udp.Client.LocalEndPoint.Port
        if ($port -gt 65534) { throw 'No usable UDP client port pair' }
        $response=Rtsp $client 'SETUP' "$uri/trackID=0" 1 "Transport: RTP/AVP;unicast;client_port=$port-$($port+1)`r`n"
        Assert ($response.Header -match '^RTSP/1.0 200' -and $response.Header -match 'server_port=') 'Secondary UDP SETUP'
        if ($response.Header -notmatch 'Session: ([^;\r\n]+)') { throw 'No UDP session' }; $session=$Matches[1]
        $response=Rtsp $client 'PLAY' $uri 2 "Session: $session`r`n"
        Assert ($response.Header -match '^RTSP/1.0 200') 'Secondary UDP PLAY'
        $frames=0; $previous=-1; $ssrc=-1L; $idr=$false
        $peer=[Net.IPEndPoint]::new([Net.IPAddress]::Any,0)
        while ($frames -lt 32) {
            $p=$udp.Receive([ref]$peer)
            if ($p.Length -lt 14 -or ($p[0] -shr 6) -ne 2) { throw 'Invalid UDP RTP' }
            $seq=([int]$p[2] -shl 8) -bor $p[3]
            if ($previous -ge 0 -and $seq -ne (($previous+1) -band 65535)) { throw 'UDP sequence gap/mixed stream' }; $previous=$seq
            $current=([long]$p[8] -shl 24) -bor ([long]$p[9] -shl 16) -bor ([long]$p[10] -shl 8) -bor $p[11]
            if ($ssrc -ge 0 -and $ssrc -ne $current) { throw 'UDP SSRC changed' }; $ssrc=$current
            $type=$p[12] -band 31
            if ($type -eq 5 -or ($type -eq 28 -and ($p[13] -band 31) -eq 5)) { $idr=$true }
            if (($p[1] -band 128) -ne 0) { $frames++ }
        }
        Assert $idr 'Secondary UDP delivers consecutive RTP and IDR frames'
        $response=Rtsp $client 'TEARDOWN' $uri 3 "Session: $session`r`n"
        Assert ($response.Header -match '^RTSP/1.0 200') 'Secondary UDP TEARDOWN'
        return $ssrc
    } finally { $client.Dispose(); $udp.Dispose() }
}
function OpenStream([string]$uri,[int]$width,[int]$height) {
    $client=[Net.Sockets.TcpClient]::new('127.0.0.1',18556)
    try {
        $client.ReceiveTimeout=5000; $client.SendTimeout=3000
        for ($attempt=0; $attempt -lt 60; $attempt++) {
            $response=Rtsp $client 'DESCRIBE' $uri 1 "Accept: application/sdp`r`n"
            if ($response.Header -match '^RTSP/1.0 200') { break }
            Start-Sleep -Milliseconds 250
        }
        Assert ($response.Header -match '^RTSP/1.0 200') "DESCRIBE $uri"
        if ($response.Body -notmatch 'sprop-parameter-sets=([^,\r\n]+),([^\r\n]+)') { throw 'Missing SPS/PPS' }
        $sps=$Matches[1]; $info=[BaselineSps]::Parse([Convert]::FromBase64String($sps))
        Assert ($info.Width -eq $width -and $info.Height -eq $height) "Real encoder SPS is ${width}x${height}"
        Write-Host "H.264: profile=$($info.Profile), level=$($info.Level), VUI FPS=$($info.VuiFps), fixed-rate=$($info.FixedFrameRate)"
        $response=Rtsp $client 'SETUP' "$uri/trackID=0" 2 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n"
        Assert ($response.Header -match '^RTSP/1.0 200') "SETUP $uri"
        if ($response.Header -notmatch 'Session: ([^;\r\n]+)') { throw 'No session' }; $session=$Matches[1]
        $response=Rtsp $client 'PLAY' $uri 3 "Session: $session`r`n"
        Assert ($response.Header -match '^RTSP/1.0 200') "PLAY $uri"
        return @{Client=$client; Previous=-1; Ssrc=-1L; Frames=0; First=-1L; Last=-1L; Idr=$false; Packets=0; Sps=$sps}
    } catch { $client.Dispose(); throw }
}
$exe=[IO.Path]::GetFullPath("$PSScriptRoot\..\x64\Release\Screen2NVR.exe")
$arguments=@('--dual-stream-test','--frames=240')
if ($Software) { $arguments=@('--software-encoder-test','--dual-stream-test','--frames=240') }
if ($Desktop) { $arguments=@('--dual-capture-test','--frames=240') }
if ($Live) {
    if ($Desktop) { throw 'Live GPU fixture requires the synthetic pipeline' }
    $arguments += '--live-settings-test'
}
$app=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
$main=$null; $sub=$null
try {
    $profiles=$null
    for ($attempt=0; $attempt -lt 60; $attempt++) {
        if ($app.HasExited) { throw 'Dual pipeline exited before becoming ready; inspect Screen2NVR.log' }
        try { $profiles=Onvif 'GetProfiles'; break } catch { Start-Sleep -Milliseconds 250 }
    }
    if (!$profiles) { throw 'ONVIF not ready' }
    $items=@($profiles.SelectNodes("//*[local-name()='Profiles']"))
    Assert ($items.Count -eq 2 -and $items[0].token -eq 'Profile_1' -and $items[1].token -eq 'Profile_2') 'ONVIF publishes main then secondary profile'
    Assert (($items[0].SelectSingleNode("*[local-name()='VideoSourceConfiguration']").token) -eq
            ($items[1].SelectSingleNode("*[local-name()='VideoSourceConfiguration']").token)) 'Both profiles share one video source'
    $caps=Onvif 'GetServiceCapabilities'
    Assert ((Node $caps 'ProfileCapabilities').MaximumNumberOfProfiles -eq '2') 'ONVIF advertises two profiles'
    foreach ($index in @(1,2)) {
        $profile=Onvif 'GetProfile' "<m:ProfileToken>Profile_$index</m:ProfileToken>"
        $config=Onvif 'GetVideoEncoderConfiguration' "<m:ConfigurationToken>VideoEncoderConfig_$index</m:ConfigurationToken>"
        $expectedWidth=if ($index -eq 1) {'1920'} else {'640'}
        Assert ((Node $config 'Width').InnerText -eq $expectedWidth -and (Node $profile 'VideoEncoderConfiguration').token -eq "VideoEncoderConfig_$index") "Profile $index has its own encoder configuration"
        $options=Onvif 'GetVideoEncoderConfigurationOptions' "<m:ProfileToken>Profile_$index</m:ProfileToken>"
        Assert ((Node $options 'Width').InnerText -eq $expectedWidth) "Profile $index options match its dimensions"
    }
    $mainUri=(Node (Onvif 'GetStreamUri' '<m:ProfileToken>Profile_1</m:ProfileToken>') 'Uri').InnerText
    $subUri=(Node (Onvif 'GetStreamUri' '<m:ProfileToken>Profile_2</m:ProfileToken>') 'Uri').InnerText
    Assert ($mainUri -match ':18556/Streaming/Channels/101$' -and $subUri -match ':18556/Streaming/Channels/102$') 'ONVIF returns the configured Hikvision-style main/sub paths on the same port'
    CheckRouting $mainUri $subUri
    Start-Sleep -Milliseconds 900
    $udpSsrc=CheckUdp $subUri
    $main=OpenStream $mainUri 1920 1080
    $sub=OpenStream $subUri 640 360
    # Consume each stream at its own cadence; alternating reads would throttle the main
    # client to the secondary FPS and fill its TCP receive window.
    $mainRead=[RtpReader]::Receive($main.Client,96)
    $subRead=[RtpReader]::Receive($sub.Client,64)
    $mainStats=$mainRead.GetAwaiter().GetResult()
    $subStats=$subRead.GetAwaiter().GetResult()
    if ($Live) {
        $osd=Onvif 'GetOSDs'
        Assert ((Node $osd 'FontSize').InnerText -eq '21' -and (Node $osd 'FontColor').Transparent -eq '0') 'ONVIF OSD reflects live font/template changes without restarting'
    }
    foreach ($field in @('Ssrc','First','Last','Frames','Packets','Idr','IdrFrames','PredictedFrames','WallSeconds','FirstIdrNalOrder')) {
        $main[$field]=$mainStats.$field; $sub[$field]=$subStats.$field
    }
    Assert ($main.Ssrc -ne $sub.Ssrc -and $main.Sps -ne $sub.Sps) 'Independent SSRC and SPS/PPS; RTP is not mixed'
    Assert ($sub.Ssrc -eq $udpSsrc) 'TCP and UDP select the same secondary stream'
    Assert ($main.Idr -and $sub.Idr) 'Both streams carry IDR frames'
    foreach ($pair in @(@($main,12),@($sub,8))) {
        $state=$pair[0]; $fps=($state.Frames-1)*90000.0/($state.Last-$state.First)
        Assert ([Math]::Abs($fps-$pair[1]) -lt 0.25) "Independent RTP frame rate: $([Math]::Round($fps,2)) fps"
        Assert ($state.IdrFrames -gt 0 -and $state.PredictedFrames -gt 0) 'Stream contains both keyframes and predicted frames'
        $rtpSeconds=($state.Last-$state.First)/90000.0
        Assert ([Math]::Abs($rtpSeconds-$state.WallSeconds) -lt 1.5) 'RTP duration matches elapsed receiving time (no accelerated timestamps)'
        Write-Host "Frames: IDR=$($state.IdrFrames), predicted=$($state.PredictedFrames); RTP seconds=$([Math]::Round($rtpSeconds,2)), wall seconds=$([Math]::Round($state.WallSeconds,2))"
        Write-Host "First keyframe NAL order: $($state.FirstIdrNalOrder) (9=AUD, 7=SPS, 8=PPS, 6=SEI, 5=IDR)"
        Assert ($state.Packets -gt 64) 'Consecutive RTP packets and complete access units received'
    }
    $main.Client.Dispose(); $main=$null
    $sub.Client.Dispose(); $sub=$null
    if (!$app.WaitForExit(45000)) { throw 'Dual pipeline did not finish' }
    Assert ($app.ExitCode -eq 0) 'Both encoders drained every submitted frame; pipeline exited cleanly'
} catch {
    if ($main) { Write-Host "Main frames: $($main.Frames), packets: $($main.Packets)" }
    if ($sub) { Write-Host "Sub frames: $($sub.Frames), packets: $($sub.Packets)" }
    throw
} finally {
    if ($main) { $main.Client.Dispose() }; if ($sub) { $sub.Client.Dispose() }
    if (!$app.HasExited) { Stop-Process -Id $app.Id -Force }
    $app.Dispose()
}
