# Read the STM32DiscVu diagnostic log from the ST-LINK Virtual COM Port.
# Usage:  .\scripts\read_com8.ps1 [-Port COM8] [-Seconds 6] [-Baud 115200]
# DTR/RTS are left de-asserted so opening the port does not disturb the target.

param(
    [string]$Port = 'COM8',
    [int]$Seconds = 6,
    [int]$Baud = 115200,
    [string]$OutFile = "$env:TEMP\com8_capture.txt"
)

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName    = $Port
$sp.BaudRate    = $Baud
$sp.Parity      = [System.IO.Ports.Parity]::None
$sp.DataBits    = 8
$sp.StopBits    = [System.IO.Ports.StopBits]::One
$sp.ReadTimeout = 1000
$sp.DtrEnable   = $false
$sp.RtsEnable   = $false

try {
    $sp.Open()
} catch {
    Write-Output ("OPEN FAILED on {0}: {1}" -f $Port, $_.Exception.Message)
    Write-Output "Is STM32CubeIDE's serial monitor, or another terminal, holding the port?"
    exit 2
}

Write-Output ("--- reading {0} @ {1} baud for {2}s (streaming to {3}) ---" -f $Port, $Baud, $Seconds, $OutFile)

# Stream every line to disk as it arrives, flushing immediately, so the capture can be
# inspected WHILE it is still running. The previous version buffered in memory and only
# wrote the file when the window closed, which meant a long capture could not be read
# early and killing it discarded everything already received.
$writer = New-Object System.IO.StreamWriter($OutFile, $false)
$writer.AutoFlush = $true

$count = 0
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    try {
        $writer.WriteLine($sp.ReadLine().TrimEnd("`r"))
        $count++
    } catch { }
}
$writer.Flush()
$writer.Close()
$sp.Close()

Write-Output ("--- captured {0} lines to {1} ---" -f $count, $OutFile)
if ($count -eq 0) {
    Write-Output "(no output - is the board running rather than halted at a breakpoint?)"
    exit 0
}
