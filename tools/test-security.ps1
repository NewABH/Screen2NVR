param([switch]$Curl, [switch]$Motherboard)
$ErrorActionPreference = 'Stop'
function Assert([bool]$condition, [string]$name) {
    if (!$condition) { throw $name }
    Write-Output "PASS: $name"
}
function Connect([int]$port, [string]$source = '127.0.0.2') {
    $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Parse($source), 0)
    $client = [Net.Sockets.TcpClient]::new($endpoint)
    $client.ReceiveTimeout = 1500
    $client.SendTimeout = 1500
    $client.Connect('127.0.0.1', $port)
    return $client
}
function Exchange($client, [string]$request) {
    $stream = $client.GetStream()
    $bytes = [Text.Encoding]::UTF8.GetBytes($request)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $response = ''
        $buffer = New-Object byte[] 8192
        while ($true) {
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if (!$count) { break }
            $response += [Text.Encoding]::UTF8.GetString($buffer, 0, $count)
            $end = $response.IndexOf("`r`n`r`n")
            if ($end -ge 0) {
                $length = 0
                if ($response.Substring(0, $end) -match '(?i)Content-Length: (\d+)') { $length = [int]$Matches[1] }
                if ([Text.Encoding]::UTF8.GetByteCount($response) -ge ($end + 4 + $length)) { break }
            }
        }
        return $response
    } catch [IO.IOException] { return '' }
}
function Rtsp([string]$authorization, [string]$source = '127.0.0.2', [int]$port = 18554) {
    $client = Connect $port $source
    try {
        $header = if ($authorization) { "Authorization: $authorization`r`n" } else { '' }
        return Exchange $client "DESCRIBE rtsp://127.0.0.1:$port/pc-screen RTSP/1.0`r`nCSeq: 1`r`n$header`r`n"
    } finally { $client.Dispose() }
}
function Http([string]$headers, [string]$body, [string]$source = '127.0.0.2', [int]$port = 18000) {
    $client = Connect $port $source
    try {
        $length = [Text.Encoding]::UTF8.GetByteCount($body)
        return Exchange $client "POST /onvif/device_service HTTP/1.1`r`nHost: localhost`r`nContent-Type: application/soap+xml`r`nContent-Length: $length`r`n$headers`r`n$body"
    } finally { $client.Dispose() }
}
function Digest([string]$inputText) {
    $md5 = [Security.Cryptography.MD5]::Create()
    try { return [BitConverter]::ToString($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($inputText))).Replace('-','').ToLowerInvariant() }
    finally { $md5.Dispose() }
}
function DigestAuthorization([string]$challenge, [string]$method, [string]$uri,
                             [string]$cnonce = 'compat-client', [string]$nc = '00000001',
                             [string]$password = 'test-password', [string]$algorithm = 'MD5', [switch]$Legacy) {
    if ($challenge -notmatch 'nonce="([^"]+)"') { throw 'Missing Digest nonce' }
    $nonce = $Matches[1]
    $ha1 = Digest "test-user:Screen2NVR:$password"
    if ($algorithm -eq 'MD5-sess') { $ha1 = Digest "${ha1}:${nonce}:$cnonce" }
    $ha2 = Digest "${method}:$uri"
    $response = if ($Legacy) { Digest "${ha1}:${nonce}:$ha2" }
        else { Digest "${ha1}:${nonce}:${nc}:${cnonce}:auth:$ha2" }
    # Exercise legal optional whitespace on both sides of '=' throughout the exchange.
    $escapedCnonce = $cnonce.Replace('\','\\').Replace('"','\"')
    $auth = "Digest username = `"test-user`", realm = `"Screen2NVR`", nonce = `"$nonce`", uri = `"$uri`", algorithm = $algorithm, response = `"$response`""
    if (!$Legacy) { $auth += ", qop = auth, nc = $nc, cnonce = `"$escapedCnonce`"" }
    return $auth
}
function Envelope([string]$token, [string]$body = '<d:GetDeviceInformation/>') {
    return '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:d="http://www.onvif.org/ver10/device/wsdl" xmlns:t="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd" xmlns:u="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"><s:Header><t:Security>' + $token + '</t:Security></s:Header><s:Body>' + $body + '</s:Body></s:Envelope>'
}
function Onvif([string]$operation, [string]$parameters = '', [string]$service = 'media', [int]$port = 18000) {
    $namespace = "http://www.onvif.org/ver10/$service/wsdl"
    $body = "<m:$operation xmlns:m=`"$namespace`">$parameters</m:$operation>"
    return Http "Authorization: $script:basic`r`n" (Envelope '' $body) '127.0.0.2' $port
}
function SoapXml([string]$response) {
    if (!$response.StartsWith('HTTP/1.1 200')) { throw 'ONVIF operation did not return HTTP 200' }
    return [xml]$response.Substring($response.IndexOf("`r`n`r`n") + 4)
}
function XmlNode($xml, [string]$name) { return $xml.SelectSingleNode("//*[local-name()='$name']") }
function DigestToken([DateTime]$time = [DateTime]::UtcNow) {
    $nonce = [Guid]::NewGuid().ToByteArray()
    $created = $time.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
    $sha = [Security.Cryptography.SHA1]::Create()
    try { $digest = [Convert]::ToBase64String($sha.ComputeHash([byte[]]($nonce + [Text.Encoding]::UTF8.GetBytes($created + 'test-password')))) }
    finally { $sha.Dispose() }
    return '<t:UsernameToken><t:Username>test-user</t:Username><t:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">' + $digest + '</t:Password><t:Nonce>' + [Convert]::ToBase64String($nonce) + '</t:Nonce><u:Created>' + $created + '</u:Created></t:UsernameToken>'
}
function Discovery([string]$source) {
    $client = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Parse($source), 0))
    $client.Client.ReceiveTimeout = 500
    try {
        $probe = [Text.Encoding]::UTF8.GetBytes('<Probe><MessageID>urn:uuid:security-test</MessageID></Probe>')
        $null = $client.Send($probe, $probe.Length, [Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 13702))
        $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        try { return [Text.Encoding]::UTF8.GetString($client.Receive([ref]$remote)) }
        catch [Net.Sockets.SocketException] { return '' }
    } finally { $client.Dispose() }
}
function StartFixture([bool]$open, [bool]$trace = $false) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = [IO.Path]::GetFullPath("$PSScriptRoot\..\x64\settings-tests\SecurityServer.exe")
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput = $true
    if ($open) { $info.Arguments = '--open' }
    if ($trace) { $info.Arguments = '--trace-test' }
    $process = [Diagnostics.Process]::Start($info)
    Start-Sleep -Milliseconds 500
    if ($process.HasExited) { throw 'Security fixture failed to start' }
    return $process
}
function StopFixture($process) {
    if (!$process.HasExited) {
        $process.StandardInput.WriteLine()
        $process.StandardInput.Flush()
        if (!$process.WaitForExit(5000)) { throw "Security fixture did not stop: $($process.Id)" }
    }
    Assert ($process.ExitCode -eq 0) 'Security fixture stopped cleanly'
    $process.Dispose()
}
$basic = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('test-user:test-password'))
$soap = Envelope ''
$fixture = StartFixture $false
try {
    Assert ((Http '' (Envelope '' '<d:GetSystemDateAndTime/>')) -match '^HTTP/1.1 200') 'ONVIF clock is available before authentication for UsernameToken synchronization'
    $httpChallenge = Http '' $soap
    Assert ($httpChallenge -match 'WWW-Authenticate: Digest ') 'ONVIF offers HTTP Digest authentication'
    Assert ($httpChallenge -match 'qop="auth"') 'ONVIF HTTP Digest still advertises qop=auth'
    if ($Motherboard) {
        $board = @(Get-CimInstance Win32_BaseBoard | Sort-Object HostingBoard -Descending)[0]
        if (!$board) { throw 'Independent motherboard information unavailable' }
        $device = SoapXml (Onvif 'GetDeviceInformation' '' 'device')
        foreach ($field in @(@('Manufacturer','Manufacturer'), @('Model','Product'), @('SerialNumber','SerialNumber'))) {
            $expected = ([string]$board.($field[1])).Trim()
            if (!$expected -or $expected.ToLowerInvariant() -in @('unknown','none','not specified','not applicable','default string','to be filled by o.e.m.','to be filled by oem','system serial number')) { $expected = 'Unknown' }
            Assert ((XmlNode $device $field[0]).InnerText -ceq $expected) "ONVIF $($field[0]) matches independent Windows motherboard information"
        }
    }
    Assert ([regex]::Matches($httpChallenge, 'WWW-Authenticate:').Count -eq 1) 'ONVIF sends an unambiguous single challenge'
    foreach ($operation in @('GetServices','GetCapabilities','GetServiceCapabilities','GetScopes','GetHostname')) {
        Assert ((Http '' (Envelope '' "<d:$operation/>")) -match '^HTTP/1.1 200') "ONVIF PRE_AUTH $operation is available without a password"
    }
    foreach ($operation in @('GetProfiles','GetStreamUri','GetVideoEncoderConfigurations','GetOSDs')) {
        Assert ((Http '' (Envelope '' "<m:$operation xmlns:m=`"http://www.onvif.org/ver10/media/wsdl`"/>")) -match '^HTTP/1.1 401') "ONVIF $operation stays password-protected"
    }
    Assert ((Http '' (Envelope '' '<x:GetSystemDateAndTime xmlns:x="urn:wrong"/>')) -match '^HTTP/1.1 401') 'Wrong namespace cannot bypass ONVIF authentication'
    Assert ((Http '' (Envelope '' '<d:GetDeviceInformation><d:Value>GetSystemDateAndTime</d:Value></d:GetDeviceInformation>')) -match '^HTTP/1.1 401') 'Text matching PRE_AUTH cannot bypass authentication'
    Assert ((Http '' (Envelope '' '<d:GetSystemDateAndTime/><d:GetDeviceInformation/>')) -match '^HTTP/1.1 401') 'Multiple SOAP operations cannot bypass authentication'
    $httpAuth = DigestAuthorization $httpChallenge 'POST' '/onvif/device_service'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 200') 'ONVIF HTTP Digest succeeds across closed HTTP connections with optional whitespace'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 401') 'ONVIF HTTP Digest replay is rejected across connections'
    $httpAuth = DigestAuthorization $httpChallenge 'POST' '/onvif/device_service' 'compat-client' '00000002'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 200') 'ONVIF HTTP Digest accepts an increasing nonce-count'
    $httpAuth = DigestAuthorization $httpChallenge 'POST' '/onvif/device_service' 'bad-password' '00000001' 'wrong-password'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 401') 'ONVIF HTTP Digest rejects a wrong password'
    $httpAuth = DigestAuthorization $httpChallenge 'POST' '/different-service' 'bad-path'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 401') 'ONVIF HTTP Digest is bound to the requested URI'
    $httpAuth = DigestAuthorization $httpChallenge 'GET' '/onvif/device_service' 'bad-method'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 401') 'ONVIF HTTP Digest is bound to the HTTP method'
    $httpAuth = DigestAuthorization $httpChallenge 'POST' '/onvif/device_service' 'peer-check'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap '127.0.0.1') -match '^HTTP/1.1 401') 'Digest nonce is bound to the challenged peer IP'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 200') 'A failed foreign-peer attempt does not consume a valid Digest count'
    $unknownAuth = DigestAuthorization 'nonce="unissued-nonce"' 'POST' '/onvif/device_service'
    Assert ((Http "Authorization: $unknownAuth`r`n" $soap) -match '^HTTP/1.1 401[\s\S]*stale=true') 'Unknown nonce is rejected with a fresh stale challenge'
    if ($Curl) {
        $result = ($soap | & curl.exe --silent --show-error --noproxy '*' --max-time 10 --http1.1 --digest --user 'test-user:test-password' --header 'Content-Type: application/soap+xml' --data-binary '@-' --include 'http://127.0.0.1:18000/onvif/device_service') -join "`n"
        Assert ($LASTEXITCODE -eq 0 -and $result -match 'HTTP/1.1 401' -and $result -match 'HTTP/1.1 200' -and $result -match 'GetDeviceInformationResponse') 'Independent curl client completes ONVIF Digest challenge/response'
        $result = ($soap | & curl.exe --silent --show-error --noproxy '*' --max-time 10 --http1.1 --digest --user 'test-user:wrong-password' --header 'Content-Type: application/soap+xml' --data-binary '@-' --include 'http://127.0.0.1:18000/onvif/device_service') -join "`n"
        Assert ($LASTEXITCODE -eq 0 -and $result -match 'HTTP/1.1 401' -and $result -notmatch 'HTTP/1.1 200') 'Independent curl client is rejected with a wrong password'
    }
    Assert ((Rtsp '') -match '^RTSP/1.0 401') 'RTSP rejects missing credentials'
    Assert ((Rtsp 'Basic d3Jvbmc6d3Jvbmc=') -match '^RTSP/1.0 401') 'RTSP rejects wrong credentials'
    Assert ((Rtsp $basic) -match '^RTSP/1.0 200') 'RTSP accepts correct Basic credentials'
    Assert ((Rtsp ($basic + 'junk')) -match '^RTSP/1.0 401') 'RTSP rejects credential prefix matches'
    Assert ((Rtsp $basic '127.0.0.3') -eq '') 'RTSP rejects IP outside allowlist'
    Assert ((Rtsp '' '127.0.0.1') -match '^RTSP/1.0 401') 'Loopback exception does not bypass authentication'
    $client = Connect 18554
    try {
        $uri = 'rtsp://127.0.0.1:18554/pc-screen'
        # A binary interleaved RTCP packet must not be parsed as an RTSP request.
        $rtcp = [byte[]](36,1,0,8,128,201,0,1,0,0,0,1)
        $client.GetStream().Write($rtcp, 0, $rtcp.Length)
        Assert ((Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $basic`r`n`r`n") -match '^RTSP/1.0 200') 'Interleaved RTCP does not corrupt RTSP framing'
        Assert ((Exchange $client "GET_PARAMETER $uri RTSP/1.0`r`nCSeq: 2`r`nAuthorization: $basic`r`nContent-Length: 4`r`n`r`nbody") -match '^RTSP/1.0 200') 'RTSP accepts a keepalive with a body'
        Assert ((Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 3`r`nAuthorization: $basic`r`n`r`n") -match '^RTSP/1.0 200') 'RTSP body does not leak into the next request'
    } finally { $client.Dispose() }
    $client = Connect 18554
    try {
        $uri = 'rtsp://127.0.0.1:18554/pc-screen'
        $challenge = Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 1`r`n`r`n"
        Assert ($challenge -match 'nonce="([^"]+)"') 'RTSP sends Digest challenge'
        $nonce = $Matches[1]
        $ha1 = Digest 'test-user:Screen2NVR:test-password'
        $ha2 = Digest "DESCRIBE:$uri"
        $response = Digest "${ha1}:${nonce}:00000001:test-cnonce:auth:$ha2"
        $auth = "Digest username=`"test-user`", realm=`"Screen2NVR`", nonce=`"$nonce`", uri=`"$uri`", qop=auth, nc=00000001, cnonce=`"test-cnonce`", response=`"$response`""
        Assert ((Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 2`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 200') 'RTSP accepts valid Digest'
        Assert ((Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 3`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 401') 'RTSP rejects repeated Digest nonce-count'
    } finally { $client.Dispose() }
    $uri = 'rtsp://127.0.0.1:18554/pc-screen'
    $challenge = Rtsp ''
    Assert ($challenge -match 'algorithm=MD5' -and $challenge -notmatch '(?i)\bqop\s*=') 'RTSP negotiates classic MD5 without qop for camera/NVR clients'
    foreach ($legacy in @($true, $false)) {
        foreach ($requestSlash in @($true, $false)) {
            $client = Connect 18554
            try {
                $cnonce = [Guid]::NewGuid().ToString()
                $requestUri = if ($requestSlash) { "$uri/" } else { $uri }
                $signedUri = if ($requestSlash) { $uri } else { "$uri/" }
                $auth = DigestAuthorization $challenge 'DESCRIBE' $uri $cnonce '00000001' -Legacy:$legacy
                Assert ((Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 200') 'NVR sequence authenticates DESCRIBE'
                $auth = DigestAuthorization $challenge 'SETUP' "$uri/trackID=0" $cnonce '00000002' -Legacy:$legacy
                $setup = Exchange $client "SETUP $uri/trackID=0 RTSP/1.0`r`nCSeq: 2`r`nAuthorization: $auth`r`nTransport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n`r`n"
                Assert ($setup -match '^RTSP/1.0 200' -and $setup -match 'Session: ([^;\r\n]+)') 'NVR sequence authenticates SETUP'
                $session = $Matches[1]
                $sequence = 2
                foreach ($method in @('PLAY', 'GET_PARAMETER', 'TEARDOWN')) {
                    $sequence++
                    if ($method -eq 'PLAY') {
                        $badAuth = DigestAuthorization $challenge $method $signedUri $cnonce ($sequence.ToString('x8')) -Legacy:$legacy -Password 'wrong-password'
                        Assert ((Exchange $client "$method $requestUri RTSP/1.0`r`nCSeq: $sequence`r`nSession: $session`r`nAuthorization: $badAuth`r`n`r`n") -match '^RTSP/1.0 401') 'Trailing-slash compatibility still rejects a wrong PLAY password'
                    }
                    $auth = DigestAuthorization $challenge $method $signedUri $cnonce ($sequence.ToString('x8')) -Legacy:$legacy
                    $response = Exchange $client "$method $requestUri RTSP/1.0`r`nCSeq: $sequence`r`nSession: $session`r`nAuthorization: $auth`r`n`r`n"
                    Assert ($response -match '^RTSP/1.0 200') "$method accepts equivalent stream URLs with/without trailing slash (legacy=$legacy, requestSlash=$requestSlash)"
                }
            } finally { $client.Dispose() }
        }
    }
    foreach ($signedUri in @("$uri/", '/pc-screen/')) {
        $auth = DigestAuthorization $challenge 'DESCRIBE' $signedUri -Legacy
        Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'DESCRIBE accepts the absolute/relative stream trailing-slash alias'
    }
    foreach ($signedUri in @('rtsp://other-host:18554/pc-screen/', 'rtsp://127.0.0.1:18555/pc-screen/',
                            '/pc-screen-sub/', '/pc-screen//', '/pc-screen/?secret=do-not-log',
                            '/pc-screen/#do-not-log', '/PC-SCREEN/', '/pc-screen/trackID=0',
                            'rtsp://test-user:test-password@127.0.0.1:18554/pc-screen/')) {
        $auth = DigestAuthorization $challenge 'DESCRIBE' $signedUri -Legacy
        Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'Trailing-slash compatibility refuses a different resource, authority, query or user-info'
    }
    $httpAuth = DigestAuthorization $httpChallenge 'POST' '/onvif/device_service/' 'http-exact-path'
    Assert ((Http "Authorization: $httpAuth`r`n" $soap) -match '^HTTP/1.1 401') 'HTTP Digest does not inherit RTSP trailing-slash aliases'
    foreach ($signedUri in @($uri, "$uri/", '/pc-screen', '/pc-screen/')) {
        $client = Connect 18554
        try {
            $auth = DigestAuthorization $challenge 'SETUP' $signedUri ([Guid]::NewGuid().ToString())
            $response = Exchange $client "SETUP $uri/trackID=0 RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $auth`r`nTransport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n`r`n"
            Assert ($response -match '^RTSP/1.0 200') "SETUP accepts Digest signed for the same stream base: $signedUri"
        } finally { $client.Dispose() }
    }
    foreach ($signedUri in @('rtsp://other-host:18554/pc-screen', '/pc-screen-sub', '/pc-screen/trackID=1', '/pc-screen?bad=1')) {
        $client = Connect 18554
        try {
            $auth = DigestAuthorization $challenge 'SETUP' $signedUri ([Guid]::NewGuid().ToString())
            $response = Exchange $client "SETUP $uri/trackID=0 RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $auth`r`nTransport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n`r`n"
            Assert ($response -match '^RTSP/1.0 401') "SETUP refuses a different authority, stream, track or query: $signedUri"
        } finally { $client.Dispose() }
    }
    $auth = DigestAuthorization $challenge 'PLAY' "$uri/" 'no-base-exception-for-play'
    $client = Connect 18554
    try {
        Assert ((Exchange $client "PLAY $uri/trackID=0 RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 401') 'Stream-base Digest exception is limited to SETUP'
    } finally { $client.Dispose() }
    $auth = DigestAuthorization $challenge 'DESCRIBE' 'rtsp://camera.example/pc-screen' 'default-port'
    $client = Connect 18554
    try {
        Assert ((Exchange $client "DESCRIBE rtsp://CAMERA.EXAMPLE:554/pc-screen RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 200') 'Digest accepts equivalent default port and hostname case'
        $auth = DigestAuthorization $challenge 'DESCRIBE' 'rtsp://camera.example:555/pc-screen' 'wrong-port'
        Assert ((Exchange $client "DESCRIBE rtsp://camera.example:554/pc-screen RTSP/1.0`r`nCSeq: 2`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 401') 'Digest refuses a different explicit port'
    } finally { $client.Dispose() }
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'folded-header'
    Assert ((Rtsp ($auth.Replace(', ',",`r`n`t"))) -match '^RTSP/1.0 200') 'RTSP accepts standards-compliant folded Digest headers'
    Assert ((Rtsp ("junk`r`nX-Other: test`r`n Authorization: $basic")) -match '^RTSP/1.0 401') 'Folded unrelated fields cannot inject credentials'
    $client = Connect 18554
    try {
        $response = Exchange $client "DESCRIBE $uri/ RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $basic`r`n`r`n"
        Assert ($response -match '^RTSP/1.0 200' -and $response.Contains("Content-Base: $uri/`r`n")) 'Trailing stream slash does not produce a doubled Content-Base slash'
        Assert ($response.Contains("a=control:*`r`nm=video")) 'SDP advertises stream-level aggregate control before the video track'
    } finally { $client.Dispose() }
    Assert ($challenge -match 'nonce="[0-9a-f]{32}"') 'Digest uses a 128-bit random hexadecimal camera-style nonce'
    Assert ([regex]::Matches($challenge, 'WWW-Authenticate:').Count -eq 1) 'RTSP sends an unambiguous single Digest challenge'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'RTSP accepts Digest on a new TCP connection and optional whitespace'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'RTSP rejects a Digest replay on another TCP connection'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'compat-client' '00000002'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'RTSP accepts nonce-count continuation after reconnect'
    $auth = DigestAuthorization $challenge 'DESCRIBE' '/pc-screen' 'relative-path'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'RTSP accepts a matching relative digest-uri'
    $auth = DigestAuthorization $challenge 'DESCRIBE' '/pc-screen-sub' 'wrong-relative-path'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'RTSP rejects a mismatched relative digest-uri'
    $auth = DigestAuthorization $challenge 'DESCRIBE' 'rtsp://other-host:18554/pc-screen' 'wrong-authority'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'RTSP rejects a mismatched absolute digest-uri'
    $auth = DigestAuthorization $challenge 'SETUP' $uri 'wrong-method'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'RTSP Digest is bound to the RTSP method'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'wrong-password' '00000001' 'wrong-password'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'RTSP Digest rejects a wrong password'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'quoted"client\value'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'Digest supports escaped quotes and backslashes in quoted fields'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'sess-client' '00000001' 'test-password' 'MD5-sess'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'RTSP accepts MD5-sess Digest'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri -Legacy
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'Legacy RTSP Digest without qop remains compatible'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri -Legacy -Password 'wrong-password'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'Legacy RTSP Digest rejects a wrong password'
    $auth = DigestAuthorization $challenge 'DESCRIBE' '/different-stream' -Legacy
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'Legacy RTSP Digest rejects a different stream URI'
    $auth = DigestAuthorization $challenge 'SETUP' $uri -Legacy
    Assert ((Rtsp $auth) -match '^RTSP/1.0 401') 'Legacy RTSP Digest is bound to the RTSP method'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri -Legacy
    Assert ((Rtsp $auth '127.0.0.1') -match '^RTSP/1.0 401') 'Legacy RTSP Digest nonce is bound to the peer IP'
    $auth = DigestAuthorization 'nonce="unissued-nonce"' 'DESCRIBE' $uri -Legacy
    $stale = Rtsp $auth
    Assert ($stale -match '^RTSP/1.0 401[\s\S]*stale=true' -and $stale -notmatch '(?i)\bqop\s*=') 'Unknown legacy nonce gets a fresh classic MD5 challenge'
    $auth = DigestAuthorization $stale 'DESCRIBE' $uri -Legacy
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'Legacy client can reconnect using the fresh nonce'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'invalid-qop-check'
    Assert ((Rtsp ($auth.Replace('qop = auth', 'qop = auth-int'))) -match '^RTSP/1.0 401') 'Unsupported Digest qop is not silently treated as legacy MD5'
    foreach ($badNc in @('1','00000000','0000000g','100000000')) {
        $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'invalid-nc-check' $badNc
        Assert ((Rtsp $auth) -match '^RTSP/1.0 401') "Invalid Digest nonce-count is rejected: $badNc"
    }
    foreach ($badCnonce in @('', ('x' * 257))) {
        $auth = DigestAuthorization $challenge 'DESCRIBE' $uri $badCnonce
        Assert ((Rtsp $auth) -match '^RTSP/1.0 401') "Invalid Digest cnonce length is rejected: $($badCnonce.Length)"
    }
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri -Legacy
    Assert ((Rtsp ($auth + ', nc=00000001')) -match '^RTSP/1.0 401') 'Digest nonce-count without qop is rejected'
    $auth = DigestAuthorization $challenge 'DESCRIBE' $uri 'malformed-check'
    Assert ((Rtsp ($auth + ', username="other"')) -match '^RTSP/1.0 401') 'Duplicate Digest fields are rejected'
    Assert ((Rtsp ($auth + ', extra="unterminated')) -match '^RTSP/1.0 401') 'Unterminated Digest quoted values are rejected'
    Assert ((Rtsp ($auth + ', extra="value"trailing')) -match '^RTSP/1.0 401') 'Trailing garbage after a quoted Digest field is rejected'
    Assert ((Rtsp $auth) -match '^RTSP/1.0 200') 'Malformed requests do not consume valid nonce-counts'
    Assert ((Http '' $soap) -match '^HTTP/1.1 401') 'ONVIF rejects missing credentials'
    Assert ((Http "Authorization: Basic d3Jvbmc6d3Jvbmc=`r`n" $soap) -match '^HTTP/1.1 401') 'ONVIF rejects wrong credentials'
    Assert ((Http "authorization: $basic`r`n" $soap) -match '^HTTP/1.1 200') 'ONVIF accepts case-insensitive Basic header'
    Assert ((Http "Authorization: ${basic}junk`r`n" $soap) -match '^HTTP/1.1 401') 'ONVIF rejects credential prefix matches'
    Assert ((Http '' ("Authorization: $basic" + $soap)) -match '^HTTP/1.1 401') 'ONVIF never reads Basic credentials from body'
    Assert ((Http "Authorization: $basic`r`n" $soap '127.0.0.3') -eq '') 'ONVIF rejects IP outside allowlist'
    $textToken = '<t:UsernameToken><t:Username>test-user</t:Username><t:Password>test-password</t:Password></t:UsernameToken>'
    Assert ((Http '' (Envelope $textToken)) -match '^HTTP/1.1 200') 'ONVIF accepts UsernameToken with alternate XML prefixes'
    Assert ((Http '' (Envelope '' $textToken)) -match '^HTTP/1.1 401') 'ONVIF rejects UsernameToken placed in SOAP body'
    Assert ((Http '' (Envelope ($textToken.Replace('test-password','wrong')))) -match '^HTTP/1.1 401') 'ONVIF rejects wrong UsernameToken password'
    Assert ((Http "Authorization: $basic`r`n" (Envelope ($textToken.Replace('test-password','wrong')))) -match '^HTTP/1.1 401') 'A valid HTTP password cannot override an invalid UsernameToken'
    Assert ((Http "Authorization: Basic d3Jvbmc6d3Jvbmc=`r`n" (Envelope $textToken)) -match '^HTTP/1.1 401') 'A valid UsernameToken cannot override invalid HTTP credentials'
    Assert ((Http "Authorization: $basic`r`nAuthorization: $basic`r`n" (Envelope $textToken)) -match '^HTTP/1.1 401') 'Duplicate HTTP credentials cannot fall back to UsernameToken'
    Assert ((Http "Authorization: $basic`r`n" (Envelope $textToken)) -match '^HTTP/1.1 200') 'Combined valid HTTP and WS-Security credentials are accepted'
    $digestXml = Envelope (DigestToken)
    Assert ((Http '' $digestXml) -match '^HTTP/1.1 200') 'ONVIF accepts SHA-1 PasswordDigest'
    Assert ((Http '' $digestXml) -match '^HTTP/1.1 401') 'ONVIF rejects replayed PasswordDigest'
    Assert ((Http '' (Envelope (DigestToken ([DateTime]::UtcNow.AddMinutes(-10))))) -match '^HTTP/1.1 401') 'ONVIF rejects expired PasswordDigest'
    Assert ((Http '' ('<!DOCTYPE x [<!ENTITY password "test-password">]>' + (Envelope ($textToken.Replace('test-password','&password;'))))) -match '^HTTP/1.1 401') 'ONVIF prohibits DTD and entity expansion'
    Assert ((Discovery '127.0.0.2') -match 'ProbeMatches') 'Discovery responds to allowed IP'
    Assert ((Discovery '127.0.0.3') -eq '') 'Discovery ignores denied IP'
    # Construct non-ASCII expected text independently of PowerShell 5 script-file encoding.
    $cameraName = (-join [char[]](0x41A,0x430,0x441,0x441,0x430,0x20,0x2116,0x35)) + ' & <' +
        (-join [char[]](0x412,0x445,0x43E,0x434)) + '> "GetProfiles"'
    $xml = SoapXml (Onvif 'GetProfiles')
    $names = @($xml.SelectNodes("//*[local-name()='Name']"))
    Assert ($names.Count -eq 3 -and @($names | Where-Object { $_.InnerText -ne $cameraName }).Count -eq 0) 'Profile and both configuration names preserve Unicode and XML special characters'
    $xml = SoapXml (Onvif 'GetProfile' '<m:ProfileToken>Profile_1</m:ProfileToken>')
    Assert ((XmlNode $xml 'Profile').GetAttribute('token') -eq 'Profile_1' -and (XmlNode $xml 'Name').InnerText -eq $cameraName) 'Individual profile exposes the camera name'
    foreach ($kind in @('Source','Encoder')) {
        $xml = SoapXml (Onvif "GetVideo${kind}Configuration" "<m:ConfigurationToken>Video${kind}Config_1</m:ConfigurationToken>")
        Assert ((XmlNode $xml 'Name').InnerText -eq $cameraName) "Individual video $kind configuration exposes the camera name"
    }
    $xml = SoapXml (Onvif 'GetScopes' '' 'device')
    $scope = @($xml.SelectNodes("//*[local-name()='ScopeItem']") | Where-Object { $_.InnerText.StartsWith('onvif://www.onvif.org/name/') })[0].InnerText
    Assert ([Uri]::UnescapeDataString($scope.Substring('onvif://www.onvif.org/name/'.Length)) -eq $cameraName) 'Discovery name scope round-trips UTF-8'
    $xml = SoapXml (Onvif 'GetOSDs')
    $osd = XmlNode $xml 'OSDs'
    Assert ($osd.GetAttribute('token') -eq 'OSD_CameraName' -and (XmlNode $xml 'PlainText').InnerText -eq $cameraName) 'OSD PlainText exposes the configured camera name'
    Assert ((XmlNode $xml 'VideoSourceConfigurationToken').InnerText -eq 'VideoSourceConfig_1') 'OSD points to the advertised video-source configuration'
    Assert ((XmlNode $xml 'FontColor').GetAttribute('Transparent') -eq '0') 'Enabled name overlay is described as visible'
    $xml = SoapXml (Onvif 'GetOSD' '<m:OSDToken>OSD_CameraName</m:OSDToken>')
    Assert ((XmlNode $xml 'PlainText').InnerText -eq $cameraName) 'Individual OSD lookup returns the same name'
    $xml = SoapXml (Onvif 'GetOSDOptions' '<m:ConfigurationToken>VideoSourceConfig_1</m:ConfigurationToken>')
    Assert ((XmlNode $xml 'MaximumNumberOfOSDs').GetAttribute('PlainText') -eq '1') 'OSD options advertise the camera-name text'
    $xml = SoapXml (Onvif 'GetServiceCapabilities')
    Assert ((XmlNode $xml 'Capabilities').GetAttribute('OSD') -eq 'true') 'Media service advertises OSD support'
    $xml = SoapXml (Onvif 'GetServiceCapabilities' '' 'device')
    Assert ((XmlNode $xml 'Security').GetAttribute('HttpDigest') -eq 'true') 'Device service advertises working HTTP Digest authentication'
    $xml = SoapXml (Onvif 'GetServices' '<m:IncludeCapability>true</m:IncludeCapability>' 'device')
    Assert ($null -ne $xml.SelectSingleNode("//*[local-name()='Capabilities' and @OSD='true']")) 'GetServices includes media capabilities when requested'
    $xml = SoapXml (Onvif 'GetHostname' '' 'device')
    Assert ((XmlNode $xml 'Name').InnerText -eq $cameraName) 'Hostname response preserves the configured friendly name'
    Assert ((Onvif 'GetOSD' '<m:OSDToken>unknown</m:OSDToken>') -match 'ter:InvalidArgVal') 'Unknown OSD token returns a fault'
    Assert ((Onvif 'GetStreamUri' '<m:ProfileToken>Profile_2</m:ProfileToken>') -match 'ter:InvalidArgVal') 'Disabled secondary ONVIF profile is not advertised as a playable stream'
    $client = Connect 18554
    try {
        Assert ((Exchange $client "DESCRIBE rtsp://127.0.0.1:18554/pc-screen-sub RTSP/1.0`r`nCSeq: 1`r`nAuthorization: $basic`r`n`r`n") -match '^RTSP/1.0 404') 'Disabled secondary RTSP path does not return main video'
    } finally { $client.Dispose() }
    Assert ((Onvif 'GetProfiles' '' 'device') -match 'ter:ActionNotSupported') 'SOAP namespace selects the correct service'
    Assert ((Onvif 'SetOSD' '<m:OSD><m:PlainText>GetProfiles</m:PlainText></m:OSD>') -match 'ter:ActionNotSupported') 'Unsupported write is not mistaken for a read by matching body text'
    Assert ((Onvif 'GetOSDs' '<m:ConfigurationToken>VideoSourceConfig_1</m:ConfigurationToken><m:ConfigurationToken>unknown</m:ConfigurationToken>') -match 'ter:InvalidArgVal') 'Ambiguous duplicate tokens are rejected'
    $xml = SoapXml (Onvif 'GetOSDs')
    Assert ((XmlNode $xml 'PlainText').InnerText -eq $cameraName) 'Remote writes never modify the local camera name'
} finally { StopFixture $fixture }
$fixture = StartFixture $false $true
try {
    $client = Connect 18554
    $udp = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Parse('127.0.0.2'), 0))
    try {
        $udp.Client.ReceiveTimeout = 2000
        $uri = 'rtsp://127.0.0.1:18554/pc-screen'
        $challenge = Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 1`r`n`r`n"
        $auth = DigestAuthorization $challenge 'DESCRIBE' $uri -Legacy
        Assert ((Exchange $client "DESCRIBE $uri RTSP/1.0`r`nCSeq: 2`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 200') 'Camera-style legacy MD5 DESCRIBE'
        $auth = DigestAuthorization $challenge 'SETUP' '/different-stream' -Legacy
        Assert ((Exchange $client "SETUP $uri/trackID=0 RTSP/1.0`r`nCSeq: 3`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 401') 'Wrong signed SETUP path is diagnosed and rejected'
        $auth = DigestAuthorization $challenge 'SETUP' "$uri/" -Legacy
        $port = $udp.Client.LocalEndPoint.Port
        $response = Exchange $client "SETUP $uri/trackID=0 RTSP/1.0`r`nCSeq: 4`r`nAuthorization: $auth`r`nTransport: RTP/AVP;unicast;client_port=$port-$($port+1)`r`n`r`n"
        Assert ($response -match '^RTSP/1.0 200') 'Camera-style legacy MD5 SETUP signs stream-base URI'
        $auth = DigestAuthorization $challenge 'PLAY' $uri -Legacy
        Assert ((Exchange $client "PLAY $uri RTSP/1.0`r`nCSeq: 5`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 200') 'Camera-style legacy MD5 PLAY'
        $peer = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        $packet = $udp.Receive([ref]$peer)
        Assert ($packet.Length -ge 14 -and ($packet[0] -shr 6) -eq 2) 'Authenticated NVR-style session actually receives RTP'
        $auth = DigestAuthorization $challenge 'TEARDOWN' $uri -Legacy
        Assert ((Exchange $client "TEARDOWN $uri RTSP/1.0`r`nCSeq: 6`r`nAuthorization: $auth`r`n`r`n") -match '^RTSP/1.0 200') 'Camera-style legacy MD5 TEARDOWN'
    } finally { $client.Dispose(); $udp.Dispose() }
} finally { StopFixture $fixture }
& "$PSScriptRoot\..\x64\settings-tests\SecurityServer.exe" --probe-test
if ($LASTEXITCODE -ne 0) { throw 'RTSP watchdog probe failed' }
$fixture = StartFixture $true
try {
    Assert ((Rtsp '' '127.0.0.3' 18555) -match '^RTSP/1.0 200') 'Disabled RTSP authentication and empty allowlist work'
    Assert ((Http '' $soap '127.0.0.3' 18001) -match '^HTTP/1.1 200') 'Disabled ONVIF authentication and empty allowlist work'
    $xml = SoapXml (Onvif 'GetOSDs' '' 'media' 18001)
    Assert ((XmlNode $xml 'PlainText').InnerText -eq $cameraName -and (XmlNode $xml 'FontColor').GetAttribute('Transparent') -eq '255') 'Name is discoverable when its video overlay is disabled'
} finally { StopFixture $fixture }
