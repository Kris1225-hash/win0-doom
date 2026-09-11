<#
type the win0doom launch command into the guest through the qemu monitor
and press enter.

the full native path is required: ccs does not resolve a bare
"win0doom.exe" from its default working directory. verified against a
live runlevel-0 boot: "\SystemRoot\system32\win0doom.exe" launches doom,
the bare name answers "is not recognized as an internal or external
command".

the hmp command is "sendkey" (singular); there is no "sendkeys". one key
per monitor command, case-sensitive names, uppercase as shift-<letter>.

examples:
  .\win0-send-launch.ps1
  .\win0-send-launch.ps1 -MonitorPort 5555
  .\win0-send-launch.ps1 -ExecutablePath '\SystemRoot\system32\other.exe'
#>
param(
    [string]$ExecutablePath = '\SystemRoot\system32\win0doom.exe',
    [string]$MonitorHost = '127.0.0.1',
    [int]$MonitorPort = 4444,
    [int]$KeyDelayMs = 80
)

$ErrorActionPreference = 'Stop'

function ConvertTo-QemuKeys {
    param([Parameter(Mandatory = $true)][string]$Text)

    $keys = @()
    foreach ($ch in $Text.ToCharArray()) {
        $s = [string]$ch
        if ($s -cmatch '[A-Z]') {
            $keys += "shift-$($s.ToLowerInvariant())"
        } elseif ($s -cmatch '[a-z0-9]') {
            $keys += $s
        } else {
            $keys += switch ($s) {
                '\' { 'backslash' }
                '.' { 'dot' }
                '-' { 'minus' }
                '_' { 'shift-minus' }
                ':' { 'shift-semicolon' }
                ' ' { 'spc' }
                default { throw "no qemu sendkey mapping for character '$s'" }
            }
        }
    }
    , $keys
}

function Read-MonitorAvailable {
    param([System.Net.Sockets.NetworkStream]$Stream, [int]$WindowMs)

    $text = New-Object System.Text.StringBuilder
    $buffer = New-Object byte[] 8192
    $deadline = (Get-Date).AddMilliseconds($WindowMs)
    while ((Get-Date) -lt $deadline) {
        if ($Stream.DataAvailable) {
            $read = $Stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { break }
            $null = $text.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
    $text.ToString()
}

$keys = ConvertTo-QemuKeys $ExecutablePath
$keys += 'ret'

$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect($MonitorHost, $MonitorPort)
    $stream = $client.GetStream()
    $null = Read-MonitorAvailable $stream 400    # drain the banner

    foreach ($key in $keys) {
        $command = "sendkey $key"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes("$command`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()

        $response = Read-MonitorAvailable $stream 60
        if ($response -match 'unknown command|invalid') {
            throw "monitor rejected '$command': $($response.Trim())"
        }
        Start-Sleep -Milliseconds $KeyDelayMs
    }
}
finally {
    $client.Dispose()
}

Write-Output "sent '$ExecutablePath' + enter ($($keys.Count) keys). allow ~10s for doom init before capturing."
