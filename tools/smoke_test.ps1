param([string]$Exe = "$PSScriptRoot\..\build\Release\CSMoyu.exe")

$signature = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CSMoyuNativeTest {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr h, string value);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
}
'@

Add-Type $signature
$process = Start-Process -FilePath $Exe -PassThru
try {
    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 20 -and $window -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        $window = $process.MainWindowHandle
    }
    if ($window -eq [IntPtr]::Zero) { throw 'GUI window not found' }
    $programMode = [CSMoyuNativeTest]::GetDlgItem($window, 1001)
    $target = [CSMoyuNativeTest]::GetDlgItem($window, 1003)
    [void][CSMoyuNativeTest]::SendMessage($programMode, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][CSMoyuNativeTest]::SetWindowText($target, '')
    $start = [CSMoyuNativeTest]::GetDlgItem($window, 1006)
    [void][CSMoyuNativeTest]::SendMessage($start, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    $alive = '{"provider":{"steamid":"local"},"player":{"steamid":"local","activity":"playing","state":{"health":100}}}'
    $dead = '{"provider":{"steamid":"local"},"player":{"steamid":"local","activity":"playing","state":{"health":0}}}'
    $teammateAlive = '{"provider":{"steamid":"local"},"player":{"steamid":"teammate","activity":"playing","state":{"health":100}}}'
    $teammateDead = '{"provider":{"steamid":"local"},"player":{"steamid":"teammate","activity":"playing","state":{"health":0}}}'
    $first = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000' -Method Post -ContentType 'application/json' -Body $alive
    $status = [CSMoyuNativeTest]::GetDlgItem($window, 1008)
    $beforeBuffer = New-Object Text.StringBuilder 256
    [void][CSMoyuNativeTest]::GetWindowText($status, $beforeBuffer, $beforeBuffer.Capacity)
    $before = $beforeBuffer.ToString()
    $second = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000' -Method Post -ContentType 'application/json' -Body $dead
    Start-Sleep -Milliseconds 250

    $text = New-Object Text.StringBuilder 256
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($first.StatusCode -ne 200 -or $second.StatusCode -ne 200) { throw 'HTTP response was not 200' }
    if ($text.ToString() -eq $before) { throw "Death transition did not change status: $text" }
    [void][CSMoyuNativeTest]::SetWindowText($status, 'teammate-ignore-sentinel')
    $deathStatus = 'teammate-ignore-sentinel'

    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000' -Method Post -ContentType 'application/json' -Body $teammateAlive)
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000' -Method Post -ContentType 'application/json' -Body $teammateDead)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -ne $deathStatus) { throw "Teammate death incorrectly changed status: $text" }

    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000' -Method Post -ContentType 'application/json' -Body $alive)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -eq $deathStatus) { throw "Respawn did not change status: $text" }
    Write-Output "PASS: own death triggered, teammate death ignored, respawn detected"
}
finally {
    if (!$process.HasExited) {
        $process.Refresh()
        $window = $process.MainWindowHandle
        if ($window -ne [IntPtr]::Zero) {
            $hotkeyMode = [CSMoyuNativeTest]::GetDlgItem($window, 1002)
            [void][CSMoyuNativeTest]::SendMessage($hotkeyMode, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
            [void][CSMoyuNativeTest]::SendMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
        $process.WaitForExit(3000) | Out-Null
    }
}
