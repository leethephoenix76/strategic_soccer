param(
    [int[]]$Budgets = @(90, 180)
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cases = (Get-Content (Join-Path $root 'tests/bot_suite_cases_v4.json') -Raw | ConvertFrom-Json).cases
$output = Join-Path $root 'build/wave_pass_suite_20261106'
New-Item -ItemType Directory -Path $output -Force | Out-Null

foreach ($case in $cases) {
    foreach ($budget in $Budgets) {
        foreach ($variant in @('pkr', 'wave')) {
            $name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-b8-t$budget"
            if ($variant -eq 'wave') {
                $name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-wave-b8-t$budget"
            }
            $log = Join-Path $output "$($case.id)_${budget}_${variant}.txt"
            if (Test-Path $log) {
                $last = (Get-Content $log -Tail 2 | Where-Object { $_.Trim() })[-1]
                $elapsed = [regex]::Match($last, ', ([\d.]+) s$').Groups[1].Value
                if ($elapsed -and [double]$elapsed -le $budget + 5) {
                    Write-Host "skip $($case.id) $budget $variant"
                    continue
                }
                Move-Item -LiteralPath $log -Destination "$log.overrun" -Force
            }
            if (Test-Path "$log.timeout") {
                Write-Host "skip timeout $($case.id) $budget $variant"
                continue
            }
            Write-Host "start $($case.id) $budget $variant $(Get-Date -Format o)"
            $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $root 'build/ssbot.exe'))
            $start.ArgumentList.Add('search')
            $start.ArgumentList.Add($case.position)
            $start.ArgumentList.Add('--bot')
            $start.ArgumentList.Add($name)
            $start.ArgumentList.Add('--time')
            $start.ArgumentList.Add([string]$budget)
            $start.WorkingDirectory = $root
            $start.UseShellExecute = $false
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $start.CreateNoWindow = $true
            $process = [Diagnostics.Process]::Start($start)
            if (-not $process.WaitForExit(($budget + 5) * 1000)) {
                $process.Kill($true)
                $process.WaitForExit()
                "hard timeout after $($budget + 5) s" | Set-Content -Path "$log.timeout" -Encoding utf8
                Write-Host "timeout $($case.id) $budget $variant $(Get-Date -Format o)"
                continue
            }
            $lines = $process.StandardOutput.ReadToEnd()
            $errors = $process.StandardError.ReadToEnd()
            if ($process.ExitCode -ne 0) { throw "search failed: $($case.id) $budget $variant $errors" }
            $lines.TrimEnd() | Set-Content -Path $log -Encoding utf8
            Write-Host "done $($case.id) $budget $variant $(Get-Date -Format o)"
        }
    }
}
