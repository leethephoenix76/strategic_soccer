$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cases = (Get-Content (Join-Path $root 'tests/bot_suite_cases_v4.json') -Raw | ConvertFrom-Json).cases
$rows = foreach ($case in $cases) {
    $lines = & (Join-Path $root 'build/ssbot.exe') reach-pass $case.position
    if ($LASTEXITCODE -ne 0) { throw "reach-pass failed: $($case.id)" }
    $mapMs = [double](($lines[0] -split ',')[1])
    $passes = @($lines[1..($lines.Length - 1)] | ConvertFrom-Csv)
    Write-Host "$($case.id): $($passes.Count) passes, map $mapMs ms"
    foreach ($pass in $passes) {
        $pass | Add-Member -NotePropertyName case -NotePropertyValue $case.id -PassThru |
            Add-Member -NotePropertyName map_ms -NotePropertyValue $mapMs -PassThru
    }
}
$columns = @('case', 'map_ms', 'target', 'claim', 'own_first', 'opponent_first',
    'path_claim', 'forecast', 'path_slot', 'path_cell', 'through', 'own_intercept',
    'opponent_intercept', 'deflect', 'race', 'race_team', 'race_ms')
$rows | Select-Object $columns | Export-Csv (Join-Path $root 'build/reach_pass_v4_20261106.csv') -NoTypeInformation -Encoding utf8
Write-Host "total: $($rows.Count) passes, $(@($rows | Where-Object { $_.path_claim -ne $_.forecast }).Count) mismatches"
