param(
    [string]$Baseline,
    [string]$Candidate,
    [double[]]$Budgets = @(90, 180),
    [string[]]$Cases = @(),
    [string]$Fixture = 'tests/bot_suite_cases_v4.json',
    [string]$OutputDirectory,
    [switch]$All,
    [switch]$List,
    [switch]$Validate
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ssbot = Join-Path $root 'build/ssbot.exe'
$fixturePath = Join-Path $root $Fixture
if (-not (Test-Path -LiteralPath $ssbot)) { throw "Missing $ssbot; run build_msvc.bat" }
if (-not (Test-Path -LiteralPath $fixturePath)) { throw "Missing fixture $fixturePath" }
$suite = Get-Content -LiteralPath $fixturePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($suite.schema -ne 1) { throw "Unsupported suite schema $($suite.schema)" }

$selected = @($suite.cases | Where-Object {
    if ($Cases.Count -gt 0) { return $Cases -contains $_.id }
    return $All -or $_.tier -eq 'core'
})
if ($selected.Count -eq 0) { throw 'No cases selected' }
foreach ($id in $Cases) {
    if (-not @($selected | Where-Object { $_.id -eq $id }).Count) { throw "Unknown case $id" }
}

# Freeze both the source record and the exact turn-start position.
foreach ($source in $suite.sources.PSObject.Properties) {
    $path = Join-Path $root $source.Name
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing source $path" }
    $digest = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($digest -ne $source.Value) { throw "Source changed: $($source.Name)" }
}
foreach ($sourceName in @($selected | Select-Object -ExpandProperty source -Unique)) {
    $path = Join-Path $root $sourceName
    $lines = @(& $ssbot positions $path)
    if ($LASTEXITCODE -ne 0) { throw "Cannot replay $sourceName" }
    foreach ($case in @($selected | Where-Object { $_.source -eq $sourceName })) {
        $prefix = "$($case.turn) action=$($case.action_index) position="
        $match = @($lines | Where-Object { $_.StartsWith($prefix) })
        if ($match.Count -ne 1 -or $match[0].Substring($prefix.Length) -ne $case.position) {
            throw "Position mismatch for $($case.id)"
        }
    }
}

if ($List -or $Validate) {
    $selected | Select-Object id, tier, turn, source, theme | Format-Table -AutoSize
    Write-Host "Validated $($selected.Count) frozen positions and $(@($suite.sources.PSObject.Properties).Count) source records."
    return
}

if (-not $Baseline -or -not $Candidate) { throw 'Specify -Baseline and -Candidate (see ssbot bots)' }
if ($Baseline -eq $Candidate) { throw 'Baseline and candidate must be different bots' }
foreach ($budget in $Budgets) { if ($budget -le 0) { throw 'Budgets must be positive' } }
$names = @(& $ssbot bots | ForEach-Object { ($_ -split '\s+')[0] })
if ($LASTEXITCODE -ne 0) { throw 'Cannot list bots' }
foreach ($name in @($Baseline, $Candidate)) {
    if ($names -notcontains $name) { throw "Unknown bot $name" }
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root ('build/bot_suite_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output directory already exists: $OutputDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$outputPath = (Resolve-Path -LiteralPath $OutputDirectory).Path
$metadata = [ordered]@{
    fixture = $Fixture
    fixture_sha256 = (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant()
    ssbot_sha256 = (Get-FileHash -LiteralPath $ssbot -Algorithm SHA256).Hash.ToLowerInvariant()
    baseline = $Baseline
    candidate = $Candidate
    budgets_seconds = @($Budgets)
    seed = $suite.seed
    threads = 1
    cases = @($selected | ForEach-Object { $_.id })
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'run.json') -Encoding UTF8
$summary = Join-Path $outputPath 'summary.csv'
$invariant = [System.Globalization.CultureInfo]::InvariantCulture

for ($caseIndex = 0; $caseIndex -lt $selected.Count; ++$caseIndex) {
    $case = $selected[$caseIndex]
    $team = $case.turn.Split('.')[1]
    foreach ($budget in $Budgets) {
        # Alternate order to reduce a systematic warm-cache advantage.
        $order = if ($caseIndex % 2 -eq 0) { @($Baseline, $Candidate) } else { @($Candidate, $Baseline) }
        foreach ($name in $order) {
            $budgetText = $budget.ToString('0.###', $invariant)
            $stem = "$($case.id)_${budgetText}s_$name"
            $record = Join-Path $outputPath ($stem + '.ssn')
            $trace = Join-Path $outputPath ($stem + '.csv')
            $white = if ($team -eq 'W') { $name } else { 'random' }
            $black = if ($team -eq 'B') { $name } else { 'random' }
            $commandArgs = @('selfplay', '--white', $white, '--black', $black,
                '--episode-turns', '1', '--maxactions', '24',
                '--start-position', $case.position, '--seed', [string]$suite.seed,
                '--threads', '1', '--time', $budgetText,
                '--out', $record, '--trace', $trace, '--silent')
            Write-Host "Running $($case.id): $name, $budgetText s"
            $runOutput = @(& $ssbot @commandArgs 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "Selfplay failed for $stem`: $($runOutput -join ' ')" }
            $replay = @(& $ssbot replay $record 2>&1)
            if ($LASTEXITCODE -ne 0) { throw "Replay failed for $stem`: $($replay -join ' ')" }
            $expectedHalfTurn = [int]$case.turn.Split('.')[0]
            if ($replay[0] -notmatch "half-turn $expectedHalfTurn$") {
                throw "Incomplete half-turn for $stem`: $($replay[0])"
            }
            $recordText = Get-Content -LiteralPath $record -Raw -Encoding UTF8
            $turnLine = [regex]::Match($recordText, '(?m)^\d+\. [WB] ([^\r\n]+)')
            if (-not $turnLine.Success) { throw "Cannot read the chosen turn in $record" }
            $rows = @(Import-Csv -LiteralPath $trace)
            if ($rows.Count -eq 0) { throw "Empty trace in $trace" }
            $rootDecision = $rows[0]
            $fullTurn = (@($rows | ForEach-Object { $_.action }) -join ' ')
            $seconds = 0.0
            foreach ($row in $rows) {
                $seconds += [double]::Parse($row.seconds, $invariant)
            }
            $result = [pscustomobject]@{
                case_id = $case.id
                source = $case.source
                turn = $case.turn
                bot = $name
                configured_beam_width = if ($name -match '-b(\d+)-') { $Matches[1] } else { '' }
                budget_s = $budgetText
                seed = $suite.seed
                chosen_halfturn = $fullTurn
                recorded_turn = $turnLine.Groups[1].Value
                thinking_s = $seconds.ToString('0.######', $invariant)
                root_reported_depth = $rootDecision.completed_turn_depth
                root_max_depth = $rootDecision.max_depth
                root_positions = $rootDecision.positions
                root_expansions = $rootDecision.expansions
                root_legal = $rootDecision.root_legal
                search_action_drops = $rootDecision.action_drops
                search_movement_drops = $rootDecision.movement_drops
                root_drop_fraction_proxy = $rootDecision.root_drop_fraction
                record = [System.IO.Path]::GetFileName($record)
                trace = [System.IO.Path]::GetFileName($trace)
            }
            $result | Export-Csv -LiteralPath $summary -NoTypeInformation -Append -Encoding UTF8
        }
    }
}
Write-Host "Completed $($selected.Count) cases at $($Budgets.Count) budgets. Summary: $summary"
