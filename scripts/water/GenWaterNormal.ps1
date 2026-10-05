# Ronin @feature 28/09/2026 DX9: generates the tileable water normal map for the WaterSea shader. docs/Water_Work.md.
#
# A height field summed from random waves whose wave vectors are whole numbers of cycles per tile, so every wave - and
# the sum - wraps exactly at the edges. The normals come from the analytic slope, not a finite difference.
#   RGB = normal * 0.5 + 0.5 (x along u, y along increasing file rows = v, z up).
#   A = foam pattern (29/09/2026): tileable cell-edge webs (Worley F2-F1, wrapped) broken into patches by a
#       low-frequency periodic wave sum. The WaterSea shader thresholds it into the shore foam band.
# Output: 32-bit uncompressed TGA, bottom-left origin, what WW3D's Targa loader expects.
#
# Usage: powershell -ExecutionPolicy Bypass -File GenWaterNormal.ps1 [-Size 512] [-Waves 96] [-Seed 1987] [-Slope 0.28]
#        [-FoamCells 90] [-KMin 3] [-KMax 40] [-KPow 1.5] [-AmpPow 1.6] [-Spread 0.87] [-Out path]
#
# Ronin @feature 02/10/2026 DX9: the wave spectrum is parameterised; the defaults write the sea's WaterNormal.tga exactly.
# The calm lake (WaterNormalLake.tga): -Waves 56 -KMin 2 -KMax 20 -AmpPow 2.0 -Spread 3.1416 - broad, smooth undulation,
# fine ripples on top, no wind: the sun gathers into one drifting highlight. The foam alpha depends only on -Seed / -FoamCells.
# The river (WaterNormalRiver.tga): -KMin 4 -KMax 48 -Spread 3.1416 - non-directional chop, a little finer than the sea:
# the flow map drags it downstream in world space, so streaks could not follow the current (02/10/2026).
# Ronin @feature 03/10/2026 DX9: plus -BankCells 60 - B holds the bank-foam field; the river shader rebuilds z from xy.
# Ronin @feature 03/10/2026 DX9: plus -LaceCells 360 - A holds an irregular current lace, shown at `seafoamtile` 80.

param(
	[int]$Size = 512,
	[int]$Waves = 96,
	[int]$Seed = 1987,
	[double]$Slope = 0.28,	# RMS surface slope; higher = rougher water
	[int]$FoamCells = 90,	# Worley feature points per tile; more = smaller foam cells
	[double]$KMin = 3.0,	# shortest frequency, cycles per tile (longest wave)
	[double]$KMax = 40.0,	# highest frequency, cycles per tile (shortest wave)
	[double]$KPow = 1.5,	# > 1 weights the picks toward the long waves
	[double]$AmpPow = 1.6,	# amplitude ~ 1 / k^this; higher = smoother, the short waves fainter
	[double]$Spread = 0.87,	# half-angle (radians) around the wind for 70% of the waves; pi = no wind
	[int]$BankCells = 0,	# Ronin @feature 03/10/2026 DX9: > 0 = B holds the river's bank-foam field (bubbles per tile), not z
	[int]$LaceCells = 0,	# Ronin @feature 03/10/2026 DX9: > 0 = A holds the river's irregular current lace (cells per tile)
	[string]$Out = "$PSScriptRoot\..\..\GeneralsMD\Code\GameEngineDevice\Source\W3DDevice\GameClient\Textures\WaterNormal.tga"
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;

public static class WaterNormalGen
{
	static double Sat(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

	// Periodic wave sum in -1..1: integer cycles per tile, so it tiles. lo..hi cycles.
	static double[] MakeWaves(Random r, int count, int lo, int hi, out int[] kx, out int[] ky)
	{
		kx = new int[count]; ky = new int[count]; double[] ph = new double[count];
		for (int i = 0; i < count; i++)
		{
			double k = lo + r.NextDouble() * (hi - lo), a = r.NextDouble() * 2.0 * Math.PI;
			kx[i] = (int)Math.Round(k * Math.Cos(a)); ky[i] = (int)Math.Round(k * Math.Sin(a));
			if (kx[i] == 0 && ky[i] == 0) kx[i] = lo;
			ph[i] = r.NextDouble() * 2.0 * Math.PI;
		}
		return ph;
	}
	static double WaveSum(double u, double v, int[] kx, int[] ky, double[] ph)
	{
		double s = 0.0;
		for (int i = 0; i < ph.Length; i++) s += Math.Sin(2.0 * Math.PI * (kx[i] * u + ky[i] * v) + ph[i]);
		return s / Math.Sqrt(ph.Length * 0.5);	// ~unit variance
	}

	// Ronin @feature 29/09/2026 DX9: the foam pattern for the alpha channel. Thin webs along the Worley cell borders (F2-F1
	// small) on warped coordinates, so the cells are curved; lines broken by a fine wave sum; big holes from a coarse one.
	// Feature points keep a minimum spacing (close pairs made bright wedges). Everything wraps, so it tiles exactly.
	static double[] Foam(int n, int seed, int cells)
	{
		Random r = new Random(seed + 17);
		double spacing = 1.0 / Math.Sqrt(cells);
		double[] px = new double[cells], py = new double[cells];
		int placed = 0;
		for (int tries = 0; placed < cells && tries < cells * 200; tries++)
		{
			double cx = r.NextDouble(), cy = r.NextDouble();
			bool ok = true;
			for (int c = 0; c < placed && ok; c++)
			{
				double dx = Math.Abs(cx - px[c]); dx = Math.Min(dx, 1.0 - dx);
				double dy = Math.Abs(cy - py[c]); dy = Math.Min(dy, 1.0 - dy);
				ok = (dx * dx + dy * dy) > (0.55 * spacing) * (0.55 * spacing);
			}
			if (ok) { px[placed] = cx; py[placed] = cy; placed++; }
		}
		int[] wx, wy, bx, by, hx, hy;
		double[] wph = MakeWaves(r, 4, 2, 4, out wx, out wy);		// warp
		double[] bph = MakeWaves(r, 10, 8, 20, out bx, out by);	// line breakup
		double[] hph = MakeWaves(r, 6, 1, 3, out hx, out hy);		// holes
		double[] f = new double[n * n];
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
			{
				double u = (x + 0.5) / n, v = (y + 0.5) / n;
				double wu = u + 0.25 * spacing * WaveSum(u, v, wx, wy, wph);
				double wv = v + 0.25 * spacing * WaveSum(v, u, wx, wy, wph);
				wu -= Math.Floor(wu); wv -= Math.Floor(wv);
				double f1 = 1e9, f2 = 1e9;
				for (int c = 0; c < placed; c++)
				{
					double dx = Math.Abs(wu - px[c]); dx = Math.Min(dx, 1.0 - dx);
					double dy = Math.Abs(wv - py[c]); dy = Math.Min(dy, 1.0 - dy);
					double d = dx * dx + dy * dy;
					if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) f2 = d;
				}
				f1 = Math.Sqrt(f1); f2 = Math.Sqrt(f2);
				double web   = Math.Pow(1.0 - Sat((f2 - f1) / (0.14 * spacing)), 1.5);
				double brk   = Sat(WaveSum(u, v, bx, by, bph) * 0.45 + 0.65);
				double holes = Sat(WaveSum(u, v, hx, hy, hph) * 0.5 + 0.55);
				f[y * n + x] = Sat(web * brk * (0.25 + 0.75 * holes) * 1.15);
			}
		return f;
	}

	// Ronin @feature 03/10/2026 DX9: a river's bank-foam field, for the blue channel. Worley F1 / spacing on warped coords
	// (independent warps per axis: one shared sheared the cells into streaks): 0 at the feature points, high at the borders.
	static double[] F1(int n, int seed, int cells)
	{
		Random r = new Random(seed);
		double spacing = 1.0 / Math.Sqrt(cells);
		double[] px = new double[cells], py = new double[cells];
		int placed = 0;
		for (int tries = 0; placed < cells && tries < cells * 200; tries++)
		{
			double cx = r.NextDouble(), cy = r.NextDouble();
			bool ok = true;
			for (int c = 0; c < placed && ok; c++)
			{
				double dx = Math.Abs(cx - px[c]); dx = Math.Min(dx, 1.0 - dx);
				double dy = Math.Abs(cy - py[c]); dy = Math.Min(dy, 1.0 - dy);
				ok = (dx * dx + dy * dy) > (0.6 * spacing) * (0.6 * spacing);
			}
			if (ok) { px[placed] = cx; py[placed] = cy; placed++; }
		}
		int[] wx, wy, vx, vy;
		double[] wph = MakeWaves(r, 5, 2, 5, out wx, out wy);
		double[] vph = MakeWaves(r, 5, 2, 5, out vx, out vy);
		double[] f = new double[n * n];
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
			{
				double u = (x + 0.5) / n, v = (y + 0.5) / n;
				double wu = u + 0.22 * spacing * WaveSum(u, v, wx, wy, wph);
				double wv = v + 0.22 * spacing * WaveSum(u, v, vx, vy, vph);
				wu -= Math.Floor(wu); wv -= Math.Floor(wv);
				double f1 = 1e9;
				for (int c = 0; c < placed; c++)
				{
					double dx = Math.Abs(wu - px[c]); dx = Math.Min(dx, 1.0 - dx);
					double dy = Math.Abs(wv - py[c]); dy = Math.Min(dy, 1.0 - dy);
					f1 = Math.Min(f1, dx * dx + dy * dy);
				}
				f[y * n + x] = Math.Sqrt(f1) / spacing;
			}
		return f;
	}

	// Ronin @feature 03/10/2026 DX9: a river's current lace for the alpha - the same thin webs, less regular: random cell sizes
	// (a small minimum spacing), a strong warp so the lines curve over several cells, the line width varying, fragments.
	static double[] Filaments(int n, int seed, int cells)
	{
		Random r = new Random(seed + 31);
		double spacing = 1.0 / Math.Sqrt(cells);
		double[] px = new double[cells], py = new double[cells];
		int placed = 0;
		for (int tries = 0; placed < cells && tries < cells * 200; tries++)
		{
			double cx = r.NextDouble(), cy = r.NextDouble();
			bool ok = true;
			for (int c = 0; c < placed && ok; c++)
			{
				double dx = Math.Abs(cx - px[c]); dx = Math.Min(dx, 1.0 - dx);
				double dy = Math.Abs(cy - py[c]); dy = Math.Min(dy, 1.0 - dy);
				ok = (dx * dx + dy * dy) > (0.35 * spacing) * (0.35 * spacing);
			}
			if (ok) { px[placed] = cx; py[placed] = cy; placed++; }
		}
		int[] wx, wy, vx, vy, lx, ly, bx, by, hx, hy;
		double[] wph = MakeWaves(r, 6, 3, 8, out wx, out wy);		// warp, u
		double[] vph = MakeWaves(r, 6, 3, 8, out vx, out vy);		// warp, v
		double[] lph = MakeWaves(r, 8, 4, 10, out lx, out ly);		// line width
		double[] bph = MakeWaves(r, 12, 12, 30, out bx, out by);	// fragments
		double[] hph = MakeWaves(r, 6, 1, 3, out hx, out hy);		// holes
		double[] f = new double[n * n];
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
			{
				double u = (x + 0.5) / n, v = (y + 0.5) / n;
				double wu = u + 0.3 * spacing * WaveSum(u, v, wx, wy, wph);
				double wv = v + 0.3 * spacing * WaveSum(u, v, vx, vy, vph);
				wu -= Math.Floor(wu); wv -= Math.Floor(wv);
				double f1 = 1e9, f2 = 1e9;
				for (int c = 0; c < placed; c++)
				{
					double dx = Math.Abs(wu - px[c]); dx = Math.Min(dx, 1.0 - dx);
					double dy = Math.Abs(wv - py[c]); dy = Math.Min(dy, 1.0 - dy);
					double d = dx * dx + dy * dy;
					if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) f2 = d;
				}
				f1 = Math.Sqrt(f1); f2 = Math.Sqrt(f2);
				double width = spacing * (0.10 + 0.08 * Sat(WaveSum(u, v, lx, ly, lph) * 0.35 + 0.5));
				double web   = Math.Pow(1.0 - Sat((f2 - f1) / width), 1.5);
				double brk   = Sat(WaveSum(u, v, bx, by, bph) * 0.35 + 0.75);
				double holes = Sat(WaveSum(u, v, hx, hy, hph) * 0.5 + 0.6);
				f[y * n + x] = Sat(web * brk * (0.6 + 0.4 * holes) * 1.15);
			}
		return f;
	}

	// Ronin @feature 03/10/2026 DX9: big and small bubbles mixed, then equalised: P(field > t) = 1 - t, so the shader's
	// coverage is the foamed fraction of the bank.
	static double[] BankField(int n, int seed, int cells, int cellsFine)
	{
		double[] a = F1(n, seed + 4242, cells), b = F1(n, seed + 777, cellsFine);
		double[] e = new double[n * n];
		int[] idx = new int[n * n];
		for (int p = 0; p < n * n; p++) { e[p] = 0.65 * a[p] + 0.35 * b[p]; idx[p] = p; }
		Array.Sort((double[])e.Clone(), idx);
		for (int i = 0; i < n * n; i++) e[idx[i]] = (i + 0.5) / (n * n);
		return e;
	}

	public static void Write(string path, int n, int waves, int seed, double slope, int foamCells,
	                         double kMin, double kMax, double kPow, double ampPow, double spread, int bankCells, int laceCells)
	{
		double[] foam = (laceCells > 0) ? Filaments(n, seed, laceCells) : Foam(n, seed, foamCells);
		double[] bank = (bankCells > 0) ? BankField(n, seed, bankCells, bankCells * 11 / 3) : null;
		Random rnd = new Random(seed);
		double[] kx = new double[waves], ky = new double[waves], amp = new double[waves], ph = new double[waves];
		for (int i = 0; i < waves; i++)
		{
			// Most waves travel within +-spread of the wind (u axis); some come from anywhere, so no stripes.
			double theta = (rnd.NextDouble() < 0.7) ? (rnd.NextDouble() * 2.0 - 1.0) * spread : rnd.NextDouble() * 2.0 * Math.PI;
			double k = kMin + Math.Pow(rnd.NextDouble(), kPow) * (kMax - kMin);	// cycles per tile, weighted to the long waves
			int ix = (int)Math.Round(k * Math.Cos(theta));
			int iy = (int)Math.Round(k * Math.Sin(theta));
			if (ix == 0 && iy == 0) ix = (int)Math.Max(1.0, Math.Round(kMin));
			double kk = Math.Sqrt(ix * ix + iy * iy);
			kx[i] = ix; ky[i] = iy;
			amp[i] = (0.5 + 0.5 * rnd.NextDouble()) / Math.Pow(kk, ampPow);	// long waves tall, short waves small
			ph[i] = rnd.NextDouble() * 2.0 * Math.PI;
		}

		double[] gx = new double[n * n], gy = new double[n * n], h = new double[n * n];
		double twoPiOverN = 2.0 * Math.PI / n;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
			{
				double sx = 0, sy = 0, sh = 0;
				for (int i = 0; i < waves; i++)
				{
					double a = twoPiOverN * (kx[i] * x + ky[i] * y) + ph[i];
					double s = Math.Sin(a);
					sh += amp[i] * Math.Cos(a);
					sx -= amp[i] * twoPiOverN * kx[i] * s;	// dh/dx per pixel
					sy -= amp[i] * twoPiOverN * ky[i] * s;	// dh/dy per pixel, y = file row
				}
				int p = y * n + x;
				gx[p] = sx; gy[p] = sy; h[p] = sh;
			}

		double sum = 0, hMin = double.MaxValue, hMax = double.MinValue;
		for (int p = 0; p < n * n; p++)
		{
			sum += gx[p] * gx[p] + gy[p] * gy[p];
			hMin = Math.Min(hMin, h[p]); hMax = Math.Max(hMax, h[p]);
		}
		double scale = slope / Math.Sqrt(sum / (n * n));	// slopes in height units per pixel -> the requested RMS slope

		using (BinaryWriter w = new BinaryWriter(File.Create(path)))
		{
			w.Write((byte)0); w.Write((byte)0); w.Write((byte)2);	// no id, no palette, uncompressed true colour
			w.Write(new byte[5]);
			w.Write((short)0); w.Write((short)0); w.Write((short)n); w.Write((short)n);
			w.Write((byte)32); w.Write((byte)0x08);	// 32 bpp, 8 alpha bits, bottom-left origin
			for (int y = 0; y < n; y++)
				for (int x = 0; x < n; x++)
				{
					int p = y * n + x;
					double nx = -gx[p] * scale, ny = -gy[p] * scale, nz = 1.0;
					double len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
					nx /= len; ny /= len; nz /= len;
					w.Write((byte)Math.Round(((bank != null) ? bank[p] : nz * 0.5 + 0.5) * 255.0));	// B - or the bank field (03/10/2026)
					w.Write((byte)Math.Round((ny * 0.5 + 0.5) * 255.0));	// G
					w.Write((byte)Math.Round((nx * 0.5 + 0.5) * 255.0));	// R
					w.Write((byte)Math.Round(foam[p] * 255.0));	// A = foam pattern (29/09/2026; was the height)
				}
		}
	}
}
'@

$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$full = [System.IO.Path]::GetFullPath($Out)
[WaterNormalGen]::Write($full, $Size, $Waves, $Seed, $Slope, $FoamCells, $KMin, $KMax, $KPow, $AmpPow, $Spread, $BankCells, $LaceCells)
Write-Output "wrote $full ($Size x $Size, $Waves waves, seed $Seed, slope $Slope, foam cells $FoamCells, k $KMin..$KMax pow $KPow, amp 1/k^$AmpPow, spread $Spread, bank cells $BankCells, lace cells $LaceCells)"
