<#
.SYNOPSIS
    Listens on the WIFI_LOGIN_PC_RECEIVER board's USB-serial port and joins
    Wi-Fi networks with `netsh` when credentials arrive.

.DESCRIPTION
    OPTIONAL ALTERNATE PATH. The PC_RECEIVER firmware's default flow no
    longer depends on this script being installed — it types a
    self-contained bootstrap command directly into Windows via its HID
    keyboard interface, so it works even with zero prior access to this PC
    (see that firmware's file header for details/caveats).

    Use THIS script instead if you get one-time physical/remote access to
    the PC before it's deployed: drop it in the Startup folder (or a
    Scheduled Task at logon) so it's already listening on every future
    request, avoiding HID keystroke injection entirely (faster, and immune
    to the "typed into the wrong window" / screen-lock failure modes of
    the HID path).

    No keyboard/UI automation is involved here: the ESP32 board is read as
    a plain USB-CDC serial device, and this script talks to Windows' own
    Wi-Fi APIs directly via `netsh`.

    Protocol (newline-terminated ASCII), matching WIFI_LOGIN_PC_RECEIVER's
    firmware:
        board -> script:  WIFI:<ssid>:<password>
        script -> board:  RESULT:OK | RESULT:FAIL:<detail>

    SSID may not contain ':'. Password is everything after the second ':'
    (may contain ':'). An empty password means an open (no-password) network.

.PARAMETER ComPort
    Serial port the board enumerated on (e.g. "COM5"). Auto-detected via the
    Espressif USB VID (303A) if omitted.

.PARAMETER BaudRate
    Must match the firmware's Serial.begin() rate. Default 115200.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File windows_wifi_provision.ps1
    powershell -ExecutionPolicy Bypass -File windows_wifi_provision.ps1 -ComPort COM5
#>

param(
    [string]$ComPort,
    [int]$BaudRate = 115200
)

$ErrorActionPreference = "Stop"

function Get-EspComPort {
    # Espressif's native USB VID is 303A. Match it in the PnP hardware ID
    # rather than the friendly name, which varies by driver/Windows version.
    $dev = Get-CimInstance -ClassName Win32_PnPEntity |
        Where-Object {
            $_.Name -match '\(COM\d+\)' -and
            ($_.PNPDeviceID -match 'VID_303A' -or $_.Name -match 'USB Serial')
        } |
        Select-Object -First 1

    if (-not $dev) { return $null }

    if ($dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

function Escape-Xml([string]$s) {
    return $s -replace '&', '&amp;' -replace '<', '&lt;' -replace '>', '&gt;' `
               -replace '"', '&quot;' -replace "'", '&apos;'
}

function New-WlanProfileXml([string]$Ssid, [string]$Password) {
    $ssidEsc = Escape-Xml $Ssid

    if ([string]::IsNullOrEmpty($Password)) {
        $security = @"
            <authEncryption>
                <authentication>open</authentication>
                <encryption>none</encryption>
                <useOneX>false</useOneX>
            </authEncryption>
"@
    } else {
        $passEsc = Escape-Xml $Password
        $security = @"
            <authEncryption>
                <authentication>WPA2PSK</authentication>
                <encryption>AES</encryption>
                <useOneX>false</useOneX>
            </authEncryption>
            <sharedKey>
                <keyType>passPhrase</keyType>
                <protected>false</protected>
                <keyMaterial>$passEsc</keyMaterial>
            </sharedKey>
"@
    }

    $xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
    <name>$ssidEsc</name>
    <SSIDConfig>
        <SSID>
            <name>$ssidEsc</name>
        </SSID>
    </SSIDConfig>
    <connectionType>ESS</connectionType>
    <connectionMode>auto</connectionMode>
    <MSM>
        <security>
$security
        </security>
    </MSM>
</WLANProfile>
"@

    $path = Join-Path $env:TEMP "wifi_login_bridge_profile.xml"
    Set-Content -Path $path -Value $xml -Encoding UTF8
    return $path
}

function Connect-Wifi([string]$Ssid, [string]$Password) {
    try {
        $xmlPath = New-WlanProfileXml -Ssid $Ssid -Password $Password

        $addOutput = netsh wlan add profile filename="$xmlPath" user=all 2>&1
        if ($LASTEXITCODE -ne 0) {
            return @{ Success = $false; Detail = "add profile failed: $addOutput" }
        }

        $connectOutput = netsh wlan connect name="$Ssid" 2>&1
        if ($LASTEXITCODE -ne 0) {
            return @{ Success = $false; Detail = "connect failed: $connectOutput" }
        }

        # Give the adapter a few seconds to actually associate.
        $connected = $false
        for ($i = 0; $i -lt 10; $i++) {
            Start-Sleep -Seconds 1
            $show = netsh wlan show interfaces
            $stateLine = $show | Select-String '^\s*State\s*:\s*(.+)$'
            $ssidLine  = $show | Select-String '^\s*SSID\s*:\s*(.+)$'
            if ($stateLine -and $ssidLine) {
                $state = $stateLine.Matches[0].Groups[1].Value.Trim()
                $curSsid = $ssidLine.Matches[0].Groups[1].Value.Trim()
                if ($state -eq 'connected' -and $curSsid -eq $Ssid) {
                    $connected = $true
                    break
                }
            }
        }

        Remove-Item $xmlPath -ErrorAction SilentlyContinue

        if ($connected) {
            return @{ Success = $true; Detail = "" }
        }
        return @{ Success = $false; Detail = "did not reach connected state within timeout" }
    } catch {
        return @{ Success = $false; Detail = "exception: $($_.Exception.Message)" }
    }
}

function Parse-WifiLine([string]$Line) {
    # WIFI:<ssid>:<password> -- ssid is between the 1st and 2nd ':',
    # password is everything after the 2nd ':' (may itself contain ':').
    if ($Line -notmatch '^WIFI:([^:]*):(.*)$') { return $null }
    return @{ Ssid = $Matches[1]; Password = $Matches[2] }
}

# ---- Resolve the COM port ----
if (-not $ComPort) {
    Write-Host "Auto-detecting the WIFI_LOGIN_PC_RECEIVER board's COM port..."
    $ComPort = Get-EspComPort
    if (-not $ComPort) {
        Write-Error "Could not find the board. Pass -ComPort explicitly (check Device Manager)."
        exit 1
    }
}
Write-Host "Using $ComPort @ $BaudRate baud."

# ---- Main listen loop. Reconnects if the port drops (e.g. board reset). ----
while ($true) {
    try {
        $port = New-Object System.IO.Ports.SerialPort $ComPort, $BaudRate, "None", 8, "One"
        $port.NewLine = "`n"
        $port.ReadTimeout = -1   # block until a line arrives
        $port.Open()
        Write-Host "Listening on $ComPort for Wi-Fi credentials..."

        while ($port.IsOpen) {
            $line = $port.ReadLine().Trim()
            if ([string]::IsNullOrEmpty($line)) { continue }

            if ($line.StartsWith("#")) {
                Write-Host "  $line"
                continue
            }

            if (-not $line.StartsWith("WIFI:")) {
                Write-Host "  (ignored) $line"
                continue
            }

            $creds = Parse-WifiLine $line
            if (-not $creds) {
                Write-Host "  malformed WIFI line: $line"
                continue
            }

            Write-Host "Received credentials for '$($creds.Ssid)'. Connecting..."
            $result = Connect-Wifi -Ssid $creds.Ssid -Password $creds.Password

            if ($result.Success) {
                Write-Host "Connected to '$($creds.Ssid)'."
                $port.WriteLine("RESULT:OK")
            } else {
                Write-Host "Failed to connect to '$($creds.Ssid)': $($result.Detail)"
                $port.WriteLine("RESULT:FAIL:$($result.Detail)")
            }
        }
    } catch {
        Write-Host "Serial error: $($_.Exception.Message). Retrying in 5s..."
    } finally {
        if ($port -and $port.IsOpen) { $port.Close() }
    }
    Start-Sleep -Seconds 5
}
