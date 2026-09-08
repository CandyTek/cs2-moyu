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
    Start-Sleep -Milliseconds 250
    $pauseMusic = [CSMoyuNativeTest]::GetDlgItem($window, 1009)
    $pauseVideo = [CSMoyuNativeTest]::GetDlgItem($window, 1010)
    if ($pauseMusic -eq [IntPtr]::Zero -or $pauseVideo -eq [IntPtr]::Zero) {
        throw 'Media pause checkboxes not found'
    }
    $helpButton = [CSMoyuNativeTest]::GetDlgItem($window, 1011)
    if ($helpButton -eq [IntPtr]::Zero) { throw 'Help button not found' }
    $programMode = [CSMoyuNativeTest]::GetDlgItem($window, 1001)
    $target = [CSMoyuNativeTest]::GetDlgItem($window, 1003)
    [void][CSMoyuNativeTest]::SendMessage($programMode, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][CSMoyuNativeTest]::SetWindowText($target, '')
    $start = [CSMoyuNativeTest]::GetDlgItem($window, 1006)
    [void][CSMoyuNativeTest]::SendMessage($start, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    $phaseLive = '{"map":{"phase":"live","round":0},"round":{"phase":"live"}}'
    $alive = '{"provider":{"steamid":"local"},"player":{"steamid":"local","activity":"playing","state":{"health":100}}}'
    $dead = '{"provider":{"steamid":"local"},"player":{"steamid":"local","activity":"playing","state":{"health":0}}}'
    $teammateAlive = '{"provider":{"steamid":"local"},"player":{"steamid":"teammate","activity":"playing","state":{"health":100}}}'
    $teammateDead = '{"provider":{"steamid":"local"},"player":{"steamid":"teammate","activity":"playing","state":{"health":0}}}'
    $nextRound = '{"map":{"phase":"live","round":1},"round":{"phase":"freezetime"}}'
    $gameOver = '{"map":{"phase":"gameover","round":1}}'
    $warmup = '{"map":{"phase":"warmup","round":0},"round":{"phase":"live"}}'
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/phase' -Method Post -ContentType 'application/json' -Body $phaseLive)
    $first = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $alive
    $status = [CSMoyuNativeTest]::GetDlgItem($window, 1008)
    $beforeBuffer = New-Object Text.StringBuilder 256
    [void][CSMoyuNativeTest]::GetWindowText($status, $beforeBuffer, $beforeBuffer.Capacity)
    $before = $beforeBuffer.ToString()
    $second = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $dead
    Start-Sleep -Milliseconds 250

    $text = New-Object Text.StringBuilder 256
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($first.StatusCode -ne 200 -or $second.StatusCode -ne 200) { throw 'HTTP response was not 200' }
    if ($text.ToString() -eq $before) { throw "Death transition did not change status: $text" }
    [void][CSMoyuNativeTest]::SetWindowText($status, 'teammate-ignore-sentinel')
    $deathStatus = 'teammate-ignore-sentinel'

    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $teammateAlive)
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $teammateDead)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -ne $deathStatus) { throw "Teammate death incorrectly changed status: $text" }

    # Restoring health in the same round must not switch back to CS2.
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $alive)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -ne $deathStatus) { throw "Health recovery incorrectly changed status: $text" }

    # The next round may be reported while spectating a teammate; round phase,
    # rather than the observed player's health, must trigger the return.
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/phase' -Method Post -ContentType 'application/json' -Body $nextRound)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -eq $deathStatus) { throw "Next round did not change status: $text" }

    # Verify game-over is the other return condition.
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/phase' -Method Post -ContentType 'application/json' -Body $phaseLive)
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $alive)
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/player' -Method Post -ContentType 'application/json' -Body $dead)
    Start-Sleep -Milliseconds 250
    [void][CSMoyuNativeTest]::SetWindowText($status, 'game-over-sentinel')
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/phase' -Method Post -ContentType 'application/json' -Body $gameOver)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -eq 'game-over-sentinel') { throw "Game over did not change status: $text" }

    # Let the three 400 ms return attempts finish, then verify warmup -> live.
    Start-Sleep -Milliseconds 1000
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/phase' -Method Post -ContentType 'application/json' -Body $warmup)
    [void][CSMoyuNativeTest]::SetWindowText($status, 'warmup-sentinel')
    [void](Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:3000/phase' -Method Post -ContentType 'application/json' -Body $phaseLive)
    Start-Sleep -Milliseconds 250
    $text.Clear() | Out-Null
    [void][CSMoyuNativeTest]::GetWindowText($status, $text, $text.Capacity)
    if ($text.ToString() -eq 'warmup-sentinel') { throw "Warmup end did not change status: $text" }
    Write-Output "PASS: death triggered; health recovery ignored; next round, game over, and warmup end detected"
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

# Closing the first instance must persist the selected radio button, and a new
# instance must restore it from settings.ini.
$verifyProcess = Start-Process -FilePath $Exe -PassThru
try {
    $verifyWindow = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 20 -and $verifyWindow -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 100
        $verifyProcess.Refresh()
        $verifyWindow = $verifyProcess.MainWindowHandle
    }
    if ($verifyWindow -eq [IntPtr]::Zero) { throw 'Persistence check window not found' }
    $restoredHotkeyMode = [CSMoyuNativeTest]::GetDlgItem($verifyWindow, 1002)
    $checked = [CSMoyuNativeTest]::SendMessage($restoredHotkeyMode, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($checked.ToInt64() -ne 1) { throw 'Radio button selection was not restored after restart' }
    Write-Output 'PASS: radio button selection persisted across restart'
}
finally {
    if (!$verifyProcess.HasExited -and $verifyWindow -ne [IntPtr]::Zero) {
        [void][CSMoyuNativeTest]::SendMessage($verifyWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        $verifyProcess.WaitForExit(3000) | Out-Null
    }
}
