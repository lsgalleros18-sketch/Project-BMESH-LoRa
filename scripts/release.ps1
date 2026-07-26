param(
    [switch]$Push,
    [string]$Version
)

$ErrorActionPreference = "Stop"

function Get-LatestSemverTag {
    $tags = git tag --list 'v[0-9]*.[0-9]*.[0-9]*' --sort=-version:refname
    if (-not $tags) {
        return $null
    }
    return $tags[0].Trim()
}

function Bump-PatchVersion {
    param([string]$CurrentVersion)

    if ([string]::IsNullOrWhiteSpace($CurrentVersion)) {
        return "1.0.0"
    }

    $parts = $CurrentVersion.TrimStart('v').Split('.')
    if ($parts.Count -ne 3) {
        throw "Version '$CurrentVersion' is not in major.minor.patch format."
    }

    $major = [int]$parts[0]
    $minor = [int]$parts[1]
    $patch = [int]$parts[2] + 1
    return "$major.$minor.$patch"
}

function Get-CommitSubjectsSinceTag {
    param([string]$SinceTag)

    $range = if ([string]::IsNullOrWhiteSpace($SinceTag)) { "HEAD" } else { "$SinceTag..HEAD" }
    $subjects = git log --pretty=format:'%s' $range
    return @($subjects | Where-Object { $_ -and $_.Trim() -ne '' })
}

function Format-ChangelogEntry {
    param(
        [string]$VersionText,
        [string[]]$Subjects
    )

    $dateText = Get-Date -Format 'yyyy-MM-dd'
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("## $VersionText - $dateText")

    if ($Subjects.Count -eq 0) {
        $lines.Add("- No commit subjects were found for this release.")
    } else {
        foreach ($subject in $Subjects) {
            $lines.Add("- $subject")
        }
    }

    return $lines
}

function Update-CMakeVersion {
    param([string]$NewVersion)

    $path = Join-Path $PSScriptRoot "..\CMakeLists.txt"
    $content = Get-Content $path -Raw
    $updated = $content -replace 'project\(ProjectLoRa VERSION [0-9]+\.[0-9]+\.[0-9]+\)', "project(ProjectLoRa VERSION $NewVersion)"
    if ($updated -eq $content) {
        throw "Could not find the project version line in CMakeLists.txt."
    }

    Set-Content -Path $path -Value $updated -NoNewline
}

function Update-Changelog {
    param(
        [string]$NewVersion,
        [string[]]$Subjects
    )

    $path = Join-Path $PSScriptRoot "..\docs\CHANGELOG.md"
    $existing = Get-Content $path -Raw
    $entry = Format-ChangelogEntry -VersionText $NewVersion -Subjects $Subjects
    $newSection = ($entry -join "`n") + "`n`n"

    if ($existing -match '^# Changelog\r?\n') {
        $body = $existing -replace '^# Changelog\r?\n', "# Changelog`n`n$($newSection)"
        Set-Content -Path $path -Value $body -NoNewline
    } else {
        throw "docs/CHANGELOG.md does not have the expected heading."
    }
}

$latestTag = Get-LatestSemverTag
$newVersion = if ($Version) { $Version.TrimStart('v') } else { Bump-PatchVersion -CurrentVersion $latestTag }
$subjects = Get-CommitSubjectsSinceTag -SinceTag $latestTag

Update-CMakeVersion -NewVersion $newVersion
Update-Changelog -NewVersion $newVersion -Subjects $subjects

git add CMakeLists.txt docs/CHANGELOG.md
git commit -m "Release v$newVersion"
git tag "v$newVersion"

if ($Push) {
    git push origin HEAD
    git push origin "v$newVersion"
}

Write-Host "Released v$newVersion"
Write-Host "Changelog updated with $($subjects.Count) commit(s)."
