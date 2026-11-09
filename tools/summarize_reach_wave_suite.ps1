$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root 'build/wave_pass_suite_20261106'
$rows = foreach ($file in Get-ChildItem $dir -Filter '*.txt') {
    $lines = @(Get-Content $file.FullName | Where-Object { $_.Trim() })
    $summary = $lines[0]
    $choice = $lines[-1]
    $parts = $file.BaseName -split '_'
    $variant = $parts[-1]
    $budget = [int]$parts[-2]
    $case = $file.BaseName.Substring(0, $file.BaseName.Length - $parts[-2].Length - $variant.Length - 2)
    $plan = [regex]::Match($choice, ': plan (.*), value ').Groups[1].Value
    $value = [regex]::Match($choice, ', value ([^,]+),').Groups[1].Value
    $depth = [regex]::Match($choice, ', depth (\d+),').Groups[1].Value
    $seconds = [regex]::Match($choice, ', ([\d.]+) s$').Groups[1].Value
    $expansions = [regex]::Match($summary, '(\d+) expansions').Groups[1].Value
    $movementDrops = [regex]::Match($summary, 'dropped movement states (\d+)').Groups[1].Value
    $actionDrops = [regex]::Match($summary, 'dropped actions (\d+)').Groups[1].Value
    [pscustomobject]@{
        case = $case
        budget = $budget
        variant = $variant
        plan = $plan
        value = $value
        depth = $depth
        seconds = $seconds
        expansions = $expansions
        movement_drops = $movementDrops
        action_drops = $actionDrops
    }
}
$rows | Sort-Object case, budget, variant | Export-Csv (Join-Path $dir 'summary.csv') -NoTypeInformation -Encoding utf8
$rows | Sort-Object case, budget, variant | Format-Table case, budget, variant, plan, value, depth, expansions -AutoSize
