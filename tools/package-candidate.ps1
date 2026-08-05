[CmdletBinding()]
param(
    # Force a from-scratch compile. Off by default: the packaged identity comes
    # from the git commit check and the SHA-256 of the installed files, not from
    # discarding object files, and a clean rebuild cost minutes on every single
    # candidate.
    [switch]$Clean
)

# Halo MCC VR is one cumulative build: Halo 3 + ODST + Halo: Reach. Reach's
# camera core is permanent while optional player-visible features fail open
# independently. There is one release preset and no Reach on/off switch. This
# stages one unaccepted local candidate under out/candidates
# after a passing build and tests, then automatically installs those
# exact manifest-verified bytes into the dedicated MCC mod directory. It never
# launches MCC and never labels rebuilt bytes as an accepted release.

$ErrorActionPreference = 'Stop'

# Native build tools (cmake, ctest) write progress and deprecation notices to
# stderr. Under ErrorActionPreference=Stop, PowerShell 5.1 turns any native
# stderr line into a terminating error, so run tool invocations with stderr
# tolerated and rely on the explicit $LASTEXITCODE checks that follow each call.
function Invoke-Tool([scriptblock]$Block) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Block } finally { $ErrorActionPreference = $saved }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$candidateRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out\candidates'))
$expectedCandidateRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out')) + [IO.Path]::DirectorySeparatorChar
$packagePreset = 'release'
$packageBuildDir = 'out\build\release'

if (-not $candidateRoot.StartsWith(
        $expectedCandidateRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Candidate path escaped the repository out directory: $candidateRoot"
}

Push-Location $repoRoot
try {
    $status = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect Git worktree state.'
    }
    if ($status.Count -ne 0) {
        throw ("Refusing to package a dirty worktree. Commit the candidate first:`n" +
            ($status -join "`n"))
    }

    $commit = (& git -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') {
        throw 'Could not resolve the candidate source commit.'
    }

    $acceptedSource = 'a5524d3fe58e4ed5507c27429ccca52a3d4fdf7d'
    & git -C $repoRoot merge-base --is-ancestor $acceptedSource $commit
    if ($LASTEXITCODE -ne 0) {
        throw "Refusing to package: HEAD does not descend from accepted source $acceptedSource."
    }

    Invoke-Tool { & cmake --preset $packagePreset }
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for preset $packagePreset."
    }

    $cachePath = Join-Path $repoRoot "$packageBuildDir\CMakeCache.txt"
    $cache = [IO.File]::ReadAllText($cachePath)
    if ($cache -notmatch
            '(?m)^HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP:BOOL=ON\r?$') {
        throw 'Refusing to package: ODST is not ON in the cumulative build.'
    }

    # Incremental. A clean rebuild was recompiling the whole tree for every
    # candidate, which is minutes per iteration for no safety: the packaged
    # identity is proven by the git commit check above plus the SHA-256 of the
    # exact installed files, not by how the object files were produced. Use
    # -Clean when a build-system change genuinely needs a from-scratch compile.
    $buildArgs = @('--build', '--preset', $packagePreset)
    if ($Clean) { $buildArgs += '--clean-first' }
    Invoke-Tool { & cmake @buildArgs }
    if ($LASTEXITCODE -ne 0) {
        throw 'Release build failed.'
    }

    Invoke-Tool { & ctest --preset $packagePreset }
    if ($LASTEXITCODE -ne 0) {
        throw 'Core tests failed.'
    }

    $finalCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
    $finalStatus =
        @(& git -C $repoRoot status --porcelain=v1 --untracked-files=normal)
    if ($LASTEXITCODE -ne 0 -or $finalCommit -ne $commit -or
            $finalStatus.Count -ne 0) {
        throw 'Repository state changed during build/test; refusing to label the artifacts.'
    }

    $createdUtc = [DateTime]::UtcNow
    $packageId = '{0}-{1}-{2}' -f $commit.Substring(0, 7), 'reach-fp-parity',
        $createdUtc.ToString("yyyyMMdd-HHmmssfff'Z'")
    $packageDir = Join-Path $candidateRoot $packageId
    if (Test-Path -LiteralPath $packageDir) {
        throw "Refusing to reuse candidate directory: $packageDir"
    }

    Invoke-Tool { & cmake --install $packageBuildDir --config Release `
        --prefix $packageDir --component dist }
    if ($LASTEXITCODE -ne 0) {
        throw 'Candidate staging failed.'
    }

    $dllPath = Join-Path $packageDir 'halo3xr.dll'
    $launcherPath = Join-Path $packageDir 'halo3xr_launcher.exe'
    foreach ($requiredPath in @(
            $dllPath,
            $launcherPath,
            (Join-Path $packageDir 'LICENSE'),
            (Join-Path $packageDir 'MANUAL-README.txt'))) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "Candidate package is missing: $requiredPath"
        }
    }

    $dll = Get-Item -LiteralPath $dllPath
    $launcher = Get-Item -LiteralPath $launcherPath
    $dllHash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash
    $launcherHash =
        (Get-FileHash -LiteralPath $launcherPath -Algorithm SHA256).Hash

    $manifest = [ordered]@{
        schema_version = 7
        status = 'UNTESTED_LOCAL_CANDIDATE'
        accepted = $false
        package_id = $packageId
        created_utc = $createdUtc.ToString('o')
        source_commit = $commit
        package_preset = $packagePreset
        titles = @('Halo 3', 'Halo 3: ODST', 'Halo: Reach')
        embedded_build_identity = [ordered]@{
            source_commit = $commit
            odst = $true
            reach = $true
            reach_render = $true
        }
        deployment_policy = [ordered]@{
            automatic_after_package = $true
            installer = 'tools/install-candidate.ps1'
            launches_mcc = $false
            changes_config = $false
        }
        # Reach support is permanent, while player-visible optional features
        # fail open independently and never disarm the working camera core.
        reach_permanent = $true
        reach_controller_input_enabled = $true
        reach_render_candidate_compiled = $true
        reach_loaded_image_preflight_enabled = $true
        reach_display_copy_readiness_enabled = $true
        reach_camera_core_enabled = $true
        reach_controller_aim_enabled = $true
        reach_two_arm_ik_guarded = $true
        reach_fp_interpolation_palette_transaction = $true
        reach_fp_h3_odst_transaction_parity_gate = $true
        reach_hrek_authored_crosshair_enabled = $true
        reach_hrek_authored_crosshair_mandatory = $true
        reach_flat_crosshair_substitute_enabled = $false
        reach_procedural_crosshair_substitute_enabled = $false
        reach_native_hud_layout_enabled = $false
        reach_projectile_alignment_enabled = $false
        reach_vehicle_view_follow_off_preserved = $true
        reach_vehicle_view_follow_render_matched_enabled = $true
        reach_vehicle_view_follow_refresh_invariant = $true
        reach_vehicle_blender_camera_defaults_enabled = $true
        reach_vehicle_retail_camera_aliases_enabled = $true
        reach_vehicle_body_hide_interval_lease_enabled = $true
        reach_native_seated_aim_reticle_enabled = $true
        reach_personal_weapon_rendered_eye_origin_enabled = $true
        reach_vehicle_barrel_origin_alignment_enabled = $false
        reach_vehicle_barrel_origin_policy = 'stock'
        reach_workshop_content_dependency = $false
        reach_fp_nested_camera_workspace = $true
        reach_fp_world_projection_execution_status = $true
        reach_forced_floating_hands = $true
        reach_copyresource_enabled = $true
        reach_engine_memory_writes_enabled = $true
        reach_runtime_hooks_enabled = $true
        base_release = 'MCC_VR_ALPHA_0.3.1'
        files = [ordered]@{
            'halo3xr.dll' = [ordered]@{
                bytes = $dll.Length
                sha256 = $dllHash
            }
            'halo3xr_launcher.exe' = [ordered]@{
                bytes = $launcher.Length
                sha256 = $launcherHash
            }
        }
        note = 'Cumulative Reach vehicle bundle: accepted View Follow OFF is preserved; View Follow ON uses the render-matched carrier basis with no refresh-rate filter; all 25 user-authored Blender camera placements plus exact retail aliases are embedded; and the native seat-bit lease hides the player body across the stereo pair and inter-frame interval. Native seated unit aim drives the Reach reticle immediately with either View Follow option; allows-weapons personal shots use the completed rendered eye, while every vehicle barrel origin remains stock. No Workshop content is required, activated, copied or redistributed. Full HUD layout and universal projectile alignment are not included. Not accepted until this exact DLL hash passes 72-144 Hz, both View Follow options, camera placement, body, authored red/green crosshair, Reach Spartan/Elite, seated personal/mounted/Covenant-turret weapon, lifecycle, and required regression tests.'
    }

    $manifestPath = Join-Path $packageDir 'CANDIDATE-MANIFEST.json'
    $json = $manifest | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText(
        $manifestPath,
        $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))

    Write-Host "Created untested candidate: $packageDir"
    Write-Host "Source:   $commit"
    Write-Host "DLL:      $dllHash"
    Write-Host "Launcher: $launcherHash"

    & powershell -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $repoRoot 'tools\install-candidate.ps1') `
        -CandidateDir $packageDir
    if ($LASTEXITCODE -ne 0) {
        throw 'Candidate was packaged but automatic installation failed.'
    }
}
finally {
    Pop-Location
}
