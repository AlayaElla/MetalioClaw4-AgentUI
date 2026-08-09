Set-StrictMode -Version Latest

function Import-AgentEspIdfEnvironment {
    param(
        [string]$IdfPath = "D:\esp\v6.0.2\esp-idf"
    )

    $ExportScript = Join-Path $IdfPath "export.ps1"
    if (-not (Test-Path -LiteralPath $ExportScript -PathType Leaf)) {
        throw "ESP-IDF export script not found: $ExportScript"
    }

    & $ExportScript | Out-Host
    if (-not $?) { throw "ESP-IDF environment setup failed." }

    $PythonCommand = Get-Command python -ErrorAction Stop
    return $PythonCommand.Source
}

function Resolve-AgentSerialPort {
    param(
        [string]$RequestedPort
    )

    $AvailablePorts = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($AvailablePorts.Count -eq 0) {
        throw "No serial ports found. Connect Agent and try again."
    }

    if (-not [string]::IsNullOrWhiteSpace($RequestedPort)) {
        $NormalizedPort = $RequestedPort.ToUpperInvariant()
        if ($AvailablePorts -notcontains $NormalizedPort) {
            throw "Serial port not found: $NormalizedPort. Available ports: $($AvailablePorts -join ', ')."
        }
        return $NormalizedPort
    }

    $PnpDevices = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\(COM\d+\)$' })
    $PortInfo = @(
        foreach ($PortName in $AvailablePorts) {
            $Device = $PnpDevices |
                Where-Object { $_.Name -match "\($([regex]::Escape($PortName))\)$" } |
                Select-Object -First 1
            $DisplayName = if ($null -ne $Device) { $Device.Name } else { $PortName }
            $IsBluetooth = $null -ne $Device -and (
                $Device.DeviceID -like "BTHENUM\*" -or
                $Device.Name -match "Bluetooth|蓝牙"
            )
            $IsEspressifP4 = $null -ne $Device -and (
                $Device.DeviceID -match "VID_303A&PID_1001" -or
                $Device.Name -match "USB Serial/JTAG"
            )
            [pscustomobject]@{
                Port = $PortName
                Name = $DisplayName
                IsBluetooth = $IsBluetooth
                IsEspressifP4 = $IsEspressifP4
            }
        }
    )

    $EspressifPorts = @($PortInfo | Where-Object { $_.IsEspressifP4 })
    if ($EspressifPorts.Count -eq 1) {
        Write-Host "Automatically selected Agent port $($EspressifPorts[0].Name)."
        return $EspressifPorts[0].Port
    }

    $PreferredPorts = @($PortInfo | Where-Object { -not $_.IsBluetooth })
    if ($PreferredPorts.Count -gt 0) {
        $Candidates = @($PreferredPorts)
    } else {
        $Candidates = @($PortInfo)
    }
    if ($Candidates.Count -eq 1) {
        Write-Host "Automatically selected $($Candidates[0].Name)."
        return $Candidates[0].Port
    }

    Write-Host "Available serial ports:"
    for ($Index = 0; $Index -lt $Candidates.Count; $Index++) {
        Write-Host ("  [{0}] {1}" -f ($Index + 1), $Candidates[$Index].Name)
    }
    $SelectionText = Read-Host "Select the Agent serial port number"
    $Selection = 0
    if (-not [int]::TryParse($SelectionText, [ref]$Selection) -or
        $Selection -lt 1 -or $Selection -gt $Candidates.Count) {
        throw "Invalid serial-port selection: $SelectionText"
    }
    return $Candidates[$Selection - 1].Port
}
