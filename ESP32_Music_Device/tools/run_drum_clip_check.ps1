# =============================================================================
#  drum_clip_check -- build + run the drum-mix "click" diagnostic.
#
#  Usage:
#      powershell -ExecutionPolicy Bypass -File .\tools\run_drum_clip_check.ps1
#      powershell -ExecutionPolicy Bypass -File .\tools\run_drum_clip_check.ps1 -Sweep
#      powershell -ExecutionPolicy Bypass -File .\tools\run_drum_clip_check.ps1 `
#                    -Gain "1.0,0.68,0.80,0.45,0.30"
#
#  WHY IT PATCHES A COPY OF ks_synth.c
#    The whole point is to find the gain headroom that removes digital clipping.
#    Sweeping gains must NOT require editing the production source each time, so
#    this script copies ks_synth.c to tools/build/ks_synth_probe.c and injects one
#    line after KS_Synth_TriggerDrum's switch:
#        if (kind < 3) d->amp *= g_probe_drum_fix[(int)kind];
#    The production file is never written.
#
#  ENCODING: ASCII-ONLY BY DESIGN.
#    Windows PowerShell 5.1 decodes a .ps1 without a BOM as ANSI, so any non-ASCII
#    byte (Chinese text, em-dash) becomes mojibake and breaks parsing. This exact
#    trap already bit this script once. Keep every byte < 0x80.
#    Therefore: the numeric sweep lives in C (drum_clip_check.c -sweep), NOT here.
# =============================================================================

param(
    [string]$Gain = "",
    [switch]$Sweep,
    [switch]$Isolate,
    [switch]$Dump,
    [switch]$AB,
    [switch]$Snare
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ToolSrc   = Join-Path $Root 'tools\drum_clip_check.c'
$KsSrc     = Join-Path $Root 'User\Middlewares\KS\ks_synth.c'
$StubDir   = Join-Path $Root 'tools\stub'
$CommonDir = Join-Path $Root 'User\Common'
$KsDir     = Join-Path $Root 'User\Middlewares\KS'
$OutDir    = Join-Path $Root 'tools\build'
$Probe     = Join-Path $OutDir 'ks_synth_probe.c'
$Exe       = Join-Path $OutDir 'drum_clip_check.exe'

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

# ---- inject the gain hook into a COPY of the synth ----
$text = [System.IO.File]::ReadAllText($KsSrc)
$text = $text -replace "`r`n", "`n"

$needle = @'
    default:
        return -1;
    }

    return 0;
}
'@
$needle = $needle -replace "`r`n", "`n"

if (-not $text.Contains($needle)) {
    Write-Host "[drum-clip] ERROR: injection point not found in ks_synth.c" -ForegroundColor Red
    Write-Host "[drum-clip]        expected the tail of KS_Synth_TriggerDrum (default/return -1/}/return 0;})"
    exit 1
}

$hook = @'
    default:
        return -1;
    }

    /* ---- [drum_clip_check probe] per-instrument gain scaling, diagnostics only ----
     * G_FIX is defined (non-static) in drum_clip_check.c and only declared here, so
     * both translation units share ONE array. Defining it here instead silently
     * gives the probe its own private copy and every -g/-sweep becomes a no-op. */
    if ((int)kind >= 0 && (int)kind < 3) d->amp *= G_FIX[(int)kind];

    return 0;
}
'@
$hook = $hook -replace "`r`n", "`n"

$patched = $text.Replace($needle, $hook)

$decl = "extern float G_FIX[3];   /* owned by drum_clip_check.c */
float g_probe_trk[3];       /* per-instrument attribution, set by the mix hook */
int   g_probe_soft = 0;     /* 1 = soft knee limiter instead of hard clamp */
float g_probe_snarelp = 0.0f;   /* snare LP coefficient; 0 = disabled */
float g_probe_snz1 = 0.0f, g_probe_snz2 = 0.0f;   /* snare LP state */"
if ($patched.Contains('#include "ds_common.h"')) {
    $patched = $patched.Replace('#include "ds_common.h"', '#include "ds_common.h"' + "`n" + $decl)
} else {
    Write-Host "[drum-clip] ERROR: ds_common.h include not found (cannot declare G_FIX)" -ForegroundColor Red
    exit 1
}

# ---- inject attribution into the drum-mix loop ----
# WHY: the full-mix sweep kept reporting ~70 clipped samples at EVERY snare fix, which
# proves the clipper is not the snare. Guessing which voice it is wastes rounds, so
# track each instrument's own contribution to the final mix and report the maxima.
$mixNeedle = @'
                mix += ks_drum_next(&s->drums[i], 1.0f / s->sample_rate) * drum_gain;
'@
$mixNeedle = $mixNeedle -replace "`r`n", "`n"

$mixHook = @'
                {
                    float dv = ks_drum_next(&s->drums[i], 1.0f / s->sample_rate) * drum_gain;
                    int  dk = (int)s->drums[i].kind;
                    mix += dv;
                    if (dk >= 0 && dk < 3) {
                        float av = dv < 0.0f ? -dv : dv;
                        if (av > g_probe_trk[dk]) g_probe_trk[dk] = av;
                    }
                }
'@
$mixHook = $mixHook -replace "`r`n", "`n"

if (-not $patched.Contains($mixNeedle)) {
    Write-Host "[drum-clip] ERROR: mix instrumentation point not found in ks_synth.c" -ForegroundColor Red
    exit 1
}
$patched = $patched.Replace($mixNeedle, $mixHook)

# ---- inject A/B limiter: g_probe_soft=1 -> soft knee instead of hard clamp ----
# WHY: the hard clamp `if (mix > 1.0f) mix = 1.0f;` turns float overflow into a hard
# corner. That corner IS a waveform discontinuity, i.e. exactly the "click" symptom.
# Rendering both ways from the same source compares damage on identical audio.
$limNeedle = @'
        if (mix > 1.0f)  mix = 1.0f;
        if (mix < -1.0f) mix = -1.0f;
'@
$limNeedle = $limNeedle -replace "`r`n", "`n"

$limHook = @'
        if (g_probe_soft) {
            /* quadratic soft knee above 0.70 — no expf/tanhf, ~4 ops, cheap on ESP32 */
            const float kn = 0.70f;
            float a = mix < 0.0f ? -mix : mix;
            if (a > kn) {
                float over = a - kn;
                a = kn + over / (1.0f + 3.3333f * over);
                if (a > 1.0f) a = 1.0f;
            }
            mix = mix < 0.0f ? -a : a;
        } else {
            if (mix > 1.0f)  mix = 1.0f;
            if (mix < -1.0f) mix = -1.0f;
        }
'@
$limHook = $limHook -replace "`r`n", "`n"

if ($patched.Contains($limNeedle)) {
    # Legacy source (hard clamp still present): inject the A/B toggle.
    $patched = $patched.Replace($limNeedle, $limHook)
} elseif ($patched.Contains('const float knee = 0.70f;')) {
    # Production already uses the soft knee, so there is nothing left to A/B: both
    # arms would be identical. Expected after the fix — just report it.
    Write-Host "[drum-clip] note: ks_synth.c already uses the soft knee; A/B arms will match."
} else {
    Write-Host "[drum-clip] ERROR: neither hard clamp nor soft knee found in ks_synth.c" -ForegroundColor Red
    exit 1
}

# ---- inject an optional snare lowpass (g_probe_snarelp: 0 = off) ----
# WHY: at fs=16k the snare's 1300Hz one-pole HIGHpass leaves the noise essentially
# full-band, so |H| at Nyquist is 0.914 — nearly all the energy sits at 8kHz, which
# reads as a click/hiss rather than a snare. A gentle LP is the only fix that reduces
# that 8kHz energy without simply turning the drum down. 0 disables it entirely.
# (The injected hook is built further below, next to the guard that skips it when
#  production already band-limits the snare.)

if ($patched.Contains('d->lp_a = 0.70f;')) {
    # Production already band-limits the snare, so the -Snare sweep cannot vary it.
    # Expected after the fix; the tool stays usable for every other measurement.
    Write-Host "[drum-clip] note: snare band-limit already active in ks_synth.c; -Snare is a no-op."
} else {
    # Match the snare pre-gain line WITHOUT hardcoding the constant: the coefficient
    # has been tuned more than once (1.6 -> 2.6), and a hardcoded needle makes this
    # whole tool abort with "snare code not found" on every retune.
    $snRe = [regex]'(?m)^[ \t]*out = hp \* [0-9.]+f \+ skin;.*$'
    $m = $snRe.Match($patched)
    if (-not $m.Success) {
        Write-Host "[drum-clip] ERROR: snare code not found in ks_synth.c" -ForegroundColor Red
        exit 1
    }
    $snNeedle = $m.Value
    $snHook = $snNeedle + "`n" + @'
        /* [drum_clip_check probe] optional snare lowpass (band-limit the 8k hiss) */
        if (g_probe_snarelp > 0.0f) {
            float a3 = g_probe_snarelp;
            g_probe_snz1 = a3 * g_probe_snz1 + (1.0f - a3) * out;
            g_probe_snz2 = a3 * g_probe_snz2 + (1.0f - a3) * g_probe_snz1;
            out = g_probe_snz2;
        }
'@
    $snHook = $snHook -replace "`r`n", "`n"
    $patched = $patched.Replace($snNeedle, $snHook)
}

[System.IO.File]::WriteAllText($Probe, $patched, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "[drum-clip] probe  = $Probe"

# ---- compile ----
$gccArgs = @(
    '-std=gnu11', '-O3', '-ffast-math', '-g', '-Wall', '-Wextra',
    "-I$StubDir", "-I$CommonDir", "-I$KsDir",
    $ToolSrc, $Probe,
    '-o', $Exe, '-lm'
)

Write-Host "[drum-clip] compiling ..."
$out = & gcc @gccArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[drum-clip] COMPILE FAILED" -ForegroundColor Red
    $out | ForEach-Object { Write-Host "  $_" }
    exit 1
}
if ($out) { $out | ForEach-Object { Write-Host "  $_" } }
Write-Host "[drum-clip] compile OK"

# ---- run ----
Write-Host ""
if ($Snare) {
    & $Exe -snare
} elseif ($AB) {
    & $Exe -ab
} elseif ($Dump) {
    & $Exe -dump
} elseif ($Isolate) {
    & $Exe -isolate
} elseif ($Sweep) {
    & $Exe -sweep
} elseif ($Gain -ne "") {
    & $Exe -g $Gain
} else {
    & $Exe
}
exit 0
