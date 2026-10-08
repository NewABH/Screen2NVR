param([int]$Frames = 7200)
$ErrorActionPreference = 'Stop'
function ReadExact($stream, [int]$size) {
    $bytes = New-Object byte[] $size
    $offset = 0
    while ($offset -lt $size) {
        $count = $stream.Read($bytes, $offset, $size - $offset)
        if (!$count) { throw 'Unexpected end of RTSP stream' }
        $offset += $count
    }
    return ,$bytes
}
function Request($stream, [string]$method, [int]$sequence, [string]$headers = '', [string]$suffix = '') {
    $basic = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('screen2nvr-test:temporary-test-password'))
    $bytes = [Text.Encoding]::ASCII.GetBytes("$method $script:uri$suffix RTSP/1.0`r`nCSeq: $sequence`r`nAuthorization: Basic $basic`r`n$headers`r`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $header = ''
    while (!$header.EndsWith("`r`n`r`n")) {
        $byte = $stream.ReadByte()
        if ($byte -lt 0 -or $header.Length -gt 8192) { throw 'Invalid RTSP response' }
        $header += [char]$byte
    }
    $body = ''
    if ($header -match '(?i)Content-Length: (\d+)') {
        $body = [Text.Encoding]::UTF8.GetString((ReadExact $stream ([int]$Matches[1])))
    }
    return $header + $body
}

# Read only the address fields. Never print credentials or save the user's INI.
$port = 554
$path = 'Streaming/Channels/101'
$section = ''
Get-Content -LiteralPath "$env:ProgramData\Screen2NVR\config.ini" | ForEach-Object {
    if ($_ -match '^\[(.+)\]$') { $section = $Matches[1] }
    if ($section -eq 'Network' -and $_ -match '^RtspPort=(\d+)') { $port = [int]$Matches[1] }
    if ($section -eq 'Network' -and $_ -match '^RtspPath=(.+)') { $path = $Matches[1].Trim() }
}
$script:uri = "rtsp://127.0.0.1:$port/$path"
$exe = [IO.Path]::GetFullPath("$PSScriptRoot\..\x64\Release\Screen2NVR.exe")
$app = Start-Process -FilePath $exe -ArgumentList '--security-test','--pipeline-test',"--frames=$Frames" -WindowStyle Hidden -PassThru
$client = $null
try {
    $response = ''
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ($app.HasExited) { throw 'Pipeline exited before RTSP became ready' }
        try {
            $client = [Net.Sockets.TcpClient]::new()
            $client.ReceiveTimeout = 3000
            $client.SendTimeout = 3000
            $client.Connect('127.0.0.1', $port)
            $stream = $client.GetStream()
            $response = Request $stream 'DESCRIBE' 1 "Accept: application/sdp`r`n"
            if ($response.StartsWith('RTSP/1.0 200')) { break }
        } catch { $response = '' }
        if ($client) { $client.Dispose(); $client = $null }
        Start-Sleep -Milliseconds 300
    }
    if (!$response.StartsWith('RTSP/1.0 200') -or $response -notmatch 'sprop-parameter-sets=.+,.+') { throw 'H.264 SDP not available' }
    Write-Output 'PASS: real encoder exposes SPS/PPS in SDP'
    $response = Request $stream 'SETUP' 2 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n" '/trackID=0'
    if (!$response.StartsWith('RTSP/1.0 200') -or $response -notmatch 'Session: ([^;\r\n]+)') { throw 'RTSP SETUP failed' }
    $session = $Matches[1]
    $response = Request $stream 'PLAY' 3 "Session: $session`r`n"
    if (!$response.StartsWith('RTSP/1.0 200')) { throw 'RTSP PLAY failed' }
    $packets = 0; $pictures = 0; $lastSequence = -1
    $hasSps = $false; $hasPps = $false; $hasIdr = $false
    while ($pictures -lt 60 -or !$hasIdr) {
        $prefix = ReadExact $stream 4
        if ($prefix[0] -ne 36 -or $prefix[1] -ne 0) { throw 'Bad RTP interleaving' }
        $size = ([int]$prefix[2] -shl 8) -bor [int]$prefix[3]
        if ($size -lt 13) { throw 'Short RTP packet' }
        $packet = ReadExact $stream $size
        if (($packet[0] -shr 6) -ne 2 -or ($packet[1] -band 127) -ne 96) { throw 'Bad RTP version or payload type' }
        $sequence = ([int]$packet[2] -shl 8) -bor [int]$packet[3]
        if ($lastSequence -ge 0 -and $sequence -ne (($lastSequence + 1) -band 65535)) { throw 'Gap in local TCP RTP sequence' }
        $lastSequence = $sequence
        $type = $packet[12] -band 31
        if ($type -eq 28 -and $size -gt 13) { $type = $packet[13] -band 31 }
        if ($type -eq 7) { $hasSps = $true }
        if ($type -eq 8) { $hasPps = $true }
        if ($type -eq 5) { $hasIdr = $true }
        if (($packet[1] -band 128) -ne 0) { $pictures++ }
        $packets++
        if ($packets -gt 30000) { throw 'No complete IDR/video frames received' }
    }
    if (!$hasSps -or !$hasPps) { throw 'Missing in-band SPS/PPS' }
    Write-Output "PASS: RTSP/TCP delivered $pictures frames, $packets consecutive RTP packets, SPS/PPS and IDR"
    $client.Dispose(); $client = $null
    $deadline = [DateTime]::UtcNow.AddMinutes(12)
    while (!$app.WaitForExit(30000)) {
        $app.Refresh()
        Write-Output ("RUNNING: private memory {0:N1} MiB; handles {1}" -f ($app.PrivateMemorySize64 / 1MB), $app.HandleCount)
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Pipeline soak timed out' }
    }
    if ($app.ExitCode -ne 0) { throw "Pipeline failed: exit $($app.ExitCode)" }
    Write-Output "PASS: $Frames encoded/published frames and clean shutdown"
} finally {
    if ($client) { $client.Dispose() }
    if (!$app.HasExited) { Stop-Process -Id $app.Id }
    $app.Dispose()
}
