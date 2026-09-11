<#
send one command to a qemu hmp monitor over tcp and print the response.

qemu must expose the monitor, e.g.:
  -monitor tcp:127.0.0.1:4444,server,nowait

examples:
  .\win0-monitor.ps1 "info version"
  .\win0-monitor.ps1 "sendkey ret"
  .\win0-monitor.ps1 "quit"
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Command,
    [string]$MonitorHost = '127.0.0.1',
    [int]$MonitorPort = 4444,
    [int]$WaitMs = 1500
)

$ErrorActionPreference = 'Stop'

# read whatever the monitor sends within a time window. raw byte reads on
# purpose: the hmp prompt "(qemu) " arrives without a trailing newline and
# would wedge any line-oriented reader.
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
            Start-Sleep -Milliseconds 100
        }
    }
    $text.ToString()
}

$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect($MonitorHost, $MonitorPort)
    $stream = $client.GetStream()

    $null = Read-MonitorAvailable $stream 400    # drain the banner

    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$Command`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()

    $response = Read-MonitorAvailable $stream $WaitMs
    if ($response) { Write-Output $response.TrimEnd() }
}
finally {
    $client.Dispose()
}
