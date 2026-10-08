$ErrorActionPreference = 'Stop'
function Assert([bool]$ok, [string]$name) {
    if (!$ok) { throw $name }; Write-Host "PASS: $name"
}
$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = [IO.Path]::GetFullPath("$PSScriptRoot\..\x64\settings-tests\SecurityServer.exe")
$info.Arguments = '--clients-test'
$info.UseShellExecute = $false; $info.CreateNoWindow = $true
$info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true
$app = [Diagnostics.Process]::Start($info)
$clients = [Collections.Generic.List[Net.Sockets.TcpClient]]::new()
$credential = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('test-user:test-password'))
function Line {
    $read = $app.StandardOutput.ReadLineAsync()
    if (!$read.Wait(5000)) { throw 'Fixture response timeout' }
    return $read.Result
}
function Connect([string]$ip) {
    $client = [Net.Sockets.TcpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Parse($ip),0))
    $client.ReceiveTimeout = 2000; $client.SendTimeout = 2000
    $client.Connect('127.0.0.1',18554); $clients.Add($client)
    return $client
}
function Request($client, [string]$method, [string]$path, [string]$headers = '') {
    $request = "$method rtsp://127.0.0.1:18554/$path RTSP/1.0`r`nCSeq: 1`r`nAuthorization: Basic $credential`r`n$headers`r`n"
    $bytes = [Text.Encoding]::ASCII.GetBytes($request); $stream = $client.GetStream()
    $stream.Write($bytes,0,$bytes.Length); $response = ''
    while (!$response.EndsWith("`r`n`r`n")) {
        $b = $stream.ReadByte(); if ($b -lt 0 -or $response.Length -gt 8192) { throw 'Bad RTSP response' }
        $response += [char]$b
    }
    if (!$response.StartsWith('RTSP/1.0 200 ')) { throw "Failed $method" }
}
function Play($client, [string]$path, [string]$agent = '') {
    $header = if ($agent) { "User-Agent: $agent`r`n" } else { '' }
    Request $client 'SETUP' "$path/trackID=0" ($header + "Transport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n")
    Request $client 'PLAY' $path $header
}
function Counts([int]$devices, [int]$main, [int]$sub, [string]$name) {
    $expected = "STATS $devices $main $sub"
    for ($attempt=0; $attempt -lt 20; $attempt++) {
        $app.StandardInput.WriteLine('stats'); $app.StandardInput.Flush()
        $actual = Line
        if ($actual -eq $expected) { Assert $true $name; return }
        Start-Sleep -Milliseconds 25
    }
    throw "$name : expected $expected, got $actual"
}
try {
    while (($ready = Line) -ne 'READY') { if ($null -eq $ready) { throw 'Fixture exited before READY' } }
    $idle = Connect '127.0.0.2'
    Counts 0 0 0 'Idle connection is not counted as a viewer'
    Request $idle 'SETUP' 'pc-screen/trackID=0' "Transport: RTP/AVP/TCP;unicast;interleaved=0-1`r`n"
    Counts 0 0 0 'SETUP alone is not counted as playback'
    Request $idle 'PLAY' 'pc-screen'
    Counts 1 1 0 'Main stream has one device and one session'
    $sub = Connect '127.0.0.2'; Play $sub 'pc-screen-sub'
    Counts 1 1 1 'One recorder reading both streams is one device, two sessions'
    $duplicate = Connect '127.0.0.2'; Play $duplicate 'pc-screen'
    Counts 1 2 1 'Additional main-stream session does not duplicate the device'
    $other = Connect '127.0.0.3'; Play $other 'pc-screen' 'Screen2NVR-HealthProbe'
    Counts 2 3 1 'Different IP is another device; remote User-Agent cannot hide it'
    $local = Connect '127.0.0.1'; Play $local 'pc-screen'
    Counts 3 4 1 'Real local player is counted'
    $probe = Connect '127.0.0.1'; Play $probe 'pc-screen' 'Screen2NVR-HealthProbe'
    Counts 3 4 1 'Internal local watchdog is excluded from all viewer counters'
    Request $sub 'TEARDOWN' 'pc-screen-sub'
    Counts 3 4 0 'Secondary TEARDOWN updates only secondary sessions'
    $duplicate.Dispose(); $idle.Dispose()
    Counts 2 2 0 'Device disappears after its last session closes'
    foreach ($client in $clients) { $client.Dispose() }
    Counts 0 0 0 'All counters return to zero after disconnect'
} finally {
    foreach ($client in $clients) { $client.Dispose() }
    if (!$app.HasExited) { $app.StandardInput.WriteLine(); $app.StandardInput.Flush() }
    if (!$app.WaitForExit(5000)) { throw 'Client fixture did not stop' }
    Assert ($app.ExitCode -eq 0) 'Client fixture exits cleanly'
    $app.Dispose()
}
