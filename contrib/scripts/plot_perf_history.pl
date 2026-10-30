#!/usr/bin/env perl

# This script renders the performance dashboard's gh-pages history YAML
# (written by .github/scripts/perf_history.pl) into a per-test runtime
# trend chart (PNG) across the PR series, mirroring the lantest and
# speedtest plotters. Rendering is delegated to gnuplot (gnuplot-nox is
# sufficient), so only core Perl modules are used.
#
# (c) 2026 Andy Lemin (andylemin)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# The history file is a YAML mapping of metric key -> list of {pr, ts,
# value} entries, one value per PR (each PR's CI run contributes a single
# mean). A per-PR snapshot therefore has no spread of its own; the spread
# only exists across the series. So each test is drawn as an expanding
# envelope over PR order:
#
#   * points: the single value each PR recorded
#   * band:   a decaying min..max envelope — collapsed to a point at the
#             first PR, pushed out whenever a PR lands outside it, and
#             otherwise relaxing exponentially back toward the rolling
#             average so one freak outlier does not pin the band open
#             forever
#   * lines:  envelope max (dashed), envelope min (dashed) and the
#             trailing 30-PR rolling average (solid) at each position in
#             the series, so the baseline follows genuine performance
#             shifts within a few PRs instead of averaging the whole
#             stored series
#
# All tests share one plot with X = PR sequence (chronological) and
# Y = mean runtime in ms (log scale, since tests span ~2ms to ~1300ms),
# lower is faster; or, with --ratio-prefix, the ratio described below.
#
# Usage:
#     plot_perf_history.pl HISTORY.yml [-o OUTPUT_BASENAME] [--title TITLE]
#                          [--subtitle SUBTITLE] [--prefix KEY_PREFIX]
#                          [--suffix KEY_SUFFIX] [--ratio-prefix KEY_PREFIX]
#                          [--ylabel LABEL]
#
# Only keys matching PREFIX...SUFFIX are plotted (defaults: "lantest." and
# ".mean_ms", i.e. the dashboard's per-test table metrics; the inline
# aggregate metrics and the speedtest MB/s series have different units and
# are skipped). Writes OUTPUT_BASENAME.png (default basename:
# "perf-history").
#
# With --ratio-prefix the chart compares two runs of the same workload,
# such as a mount under test and a control mount measured in the same CI
# run: each test's value at a PR is the value stored under RATIO_PREFIX
# over the value stored under PREFIX for that PR, both from one merge
# (equal timestamps; a PR holding one side, or a zero, is skipped). The
# ratio series are drawn as plain lines through the per-PR points, without
# envelopes, over a thick black line at 1.00, the parity between the two
# runs; the orientation decides which side of the line is faster, which
# --ylabel should say.

use strict;
use warnings;
use Getopt::Long qw(GetOptions);

# Near-white canvas (RGB 246,248,250) for a clean modern background.
my $BACKGROUND_COLOR = '#F6F8FA';

# Wider than the sibling plotters' 590px: the right margin hosts a legend
# row per test series.
my $WIDTH_PX  = 1430;
my $HEIGHT_PX = 780;

# One stable color per series, assigned in legend order. Tab10 first, then
# its pastel pairs and a few darker reserves so ~22 lantest tests stay
# distinguishable.
my @PALETTE = (
               '#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd',
               '#8c564b', '#e377c2', '#7f7f7f', '#bcbd22', '#17becf',
               '#aec7e8', '#ffbb78', '#98df8a', '#ff9896', '#c5b0d5',
               '#c49c94', '#f7b6d2', '#393b79', '#637939', '#8c6d31',
               '#843c39', '#7b4173',
);

# Map each lantest metric key (matched by a substring) to a short legend
# label, mirroring plot_lantest.pl's axis labels. Order matters: first
# matching substring wins.
my @SHORT_LABELS = (
                    ['writing_one_large_file'    => 'Write 100MB'],
                    ['reading_one_large_file'    => 'Read 100MB'],
                    ['creating_2000_files'       => 'Create files 2k'],
                    ['open_write_1024'           => 'Write 2k'],
                    ['open_read_1024'            => 'Read 2k'],
                    ['copying_1000_files_client' => 'Copy (R+W) 1k'],
                    ['copying_2000_files_server' => 'ServerCopy 2k'],
                    ['lock_then_unlock_2000'     => 'Fork lock 2k'],
                    ['stat_lookup_getparams'     => 'Stat 2k'],
                    ['enumerate_dir'             => 'Enumerate 2k'],
                    ['deleting_2000_files'       => 'Delete 2k'],
                    ['byte_range_lock_unlock'    => 'Byte lock 2k'],
                    ['create_2000_dirs_tree'     => 'Create Dirs 2k'],
                    ['directory_cache_hits'      => 'Dircache hits'],
                    ['mixed_cache_operations'    => 'Mixed cache'],
                    ['deep_path_traversal'       => 'Deep traverse'],
                    ['cache_validation'          => 'Cache validate'],
);

# Map a metric key to a short legend label, falling back to the key's
# middle segment with underscores spaced if no keyword matches.
sub short_label {
    my ($key) = @_;
    for my $pair (@SHORT_LABELS) {
        my ($needle, $label) = @$pair;
        return $label if index($key, $needle) >= 0;
    }
    my $stripped = $key;
    $stripped =~ s/^[^.]*\.//;
    $stripped =~ s/\.[^.]*$//;
    $stripped =~ s/_/ /g;
    return $stripped;
}

# Read the history YAML: a mapping of key -> list of {pr, ts, value},
# the exact shape perf_history.pl writes. Unparseable keys or entries are
# dropped rather than fatal, mirroring that script's tolerant reader.
sub load_history {
    my ($path) = @_;
    my %hist;
    open my $fh, '<:encoding(UTF-8)', $path or die "cannot open $path: $!\n";
    my $key;
    my $entry;
    while (my $line = <$fh>) {
        chomp $line;
        next if $line =~ /^\s*(?:#.*)?$/;
        if ($line =~ /^(\S.*?):\s*$/) {
            $key        = _yaml_unquote($1);
            $entry      = undef;
            $hist{$key} = [];
        } elsif (defined $key && $line =~ /^- (\w+): (.*)$/) {
            $entry = {};
            push @{$hist{$key}}, $entry;
            $entry->{$1} = _yaml_unquote($2);
        } elsif (defined $entry && $line =~ /^  (\w+): (.*)$/) {
            $entry->{$1} = _yaml_unquote($2);
        } else {
            delete $hist{$key} if defined $key;
            $key = $entry = undef;
        }
    }
    close $fh;
    for my $k (keys %hist) {
        $hist{$k} =
          [grep { defined $_->{pr} && defined $_->{value} && $_->{value} =~ /^-?\d+(?:\.\d+)?$/ } @{$hist{$k}}];
    }
    return \%hist;
}

# A YAML scalar without the quotes perf_history.pl may have added.
sub _yaml_unquote {
    my ($s) = @_;
    $s =~ s/^'(.*)'$/$1/;
    $s =~ s/^"(.*)"$/$1/;
    return $s;
}

# Escape a string for a double-quoted gnuplot string (newline -> \n escape).
sub gp_quote {
    my ($s) = @_;
    $s =~ s/\\/\\\\/g;
    $s =~ s/"/\\"/g;
    $s =~ s/\n/\\n/g;
    return $s;
}

# Global PR order: unique PRs across all selected series, sorted by the
# earliest timestamp any series recorded for them (ISO 8601 sorts
# lexically), so every series shares one x position per PR even when a
# series is missing some PRs.
sub pr_positions {
    my ($series) = @_;
    my %first_ts;
    for my $entries (values %$series) {
        for my $e (@$entries) {
            my $ts = $e->{ts} // '';
            $first_ts{$e->{pr}} = $ts
              if !exists $first_ts{$e->{pr}} || $ts lt $first_ts{$e->{pr}};
        }
    }
    my @prs = sort { ($first_ts{$a} cmp $first_ts{$b}) || ($a <=> $b) }
      keys %first_ts;
    my %pos = map { $prs[$_] => $_ } 0 .. $#prs;
    return (\@prs, \%pos);
}

# The parity line of a ratio chart, black so every test keeps the color it
# has in the runtime chart.
my $PARITY_COLOR = '#000000';

# $opts: ratio => 1 for a ratio chart, ylabel => the y-axis label.
sub plot {
    my ($series, $out_base, $title, $subtitle, $opts) = @_;
    my @keys = sort { short_label($a) cmp short_label($b) || $a cmp $b }
      grep { @{$series->{$_}} } keys %$series;
    if (!@keys) {
        print STDERR "ERROR: no matching history series found — is this a " . "perf_history.pl history YAML?\n";
        return 0;
    }
    my $ratio = $opts->{ratio};

    my ($prs, $pos) = pr_positions($series);
    my $n = scalar @$prs;

    # Per-step decay factor for the envelope edges: after a new extreme,
    # each subsequent in-band PR moves the edge this fraction of its
    # remaining distance back toward the rolling average (0.15 ≈ half
    # the excursion forgotten after ~4 PRs, fully faded after ~20 — one
    # fifth of the 100-entry series cap).
    my $DECAY = 0.15;

    # Trailing window for the solid average line, matching perf_history.pl's
    # table baseline: long enough to smooth runner variance, short enough
    # that an accepted performance change becomes the new baseline within
    # weeks instead of being diluted by the whole stored series.
    my $AVG_WINDOW = 30;

    # Columns per series row: x, value, env_min, env_max, avg. The stats
    # are computed left-to-right: the envelope opens when a value lands
    # outside it and decays toward the average while values stay inside.
    my $blocks = '';
    my (@bands, @maxlines, @minlines, @avglines, @points, @keyentries);
    my ($data_min, $data_max);
    my $idx = 0;
    for my $key (@keys) {
        my @entries = sort { ($pos->{$a->{pr}} // 0) <=> ($pos->{$b->{pr}} // 0) } @{$series->{$key}};
        my $color   = $PALETTE[$idx % @PALETTE];
        my $light   = '#80' . substr($color, 1);
        my $tag     = "S$idx";
        $blocks .= "\$$tag << EOD\n";
        my ($env_min, $env_max, $win_sum, @win) = (undef, undef, 0);
        for my $e (@entries) {
            my $v = $e->{value} + 0;
            push @win, $v;
            $win_sum += $v;
            $win_sum -= shift @win if @win > $AVG_WINDOW;
            my $avg = $win_sum / @win;
            # Relax each edge part-way toward the average, then push it
            # back out if this PR's value lands beyond it.
            if (defined $env_min) {
                $env_min += $DECAY * ($avg - $env_min) if $env_min < $avg;
                $env_max -= $DECAY * ($env_max - $avg) if $env_max > $avg;
            }
            $env_min = $v if !defined $env_min || $v < $env_min;
            $env_max = $v if !defined $env_max || $v > $env_max;
            my $x = $pos->{$e->{pr}};
            $blocks .= "$x $v $env_min $env_max $avg\n";
            $data_min = $v if !defined $data_min || $v < $data_min;
            $data_max = $v if !defined $data_max || $v > $data_max;
        }
        $blocks .= "EOD\n";

        if ($ratio) {
            # One line through the per-PR ratios; the ratio already cancels
            # the runner's speed, so no envelope or rolling average.
            push @avglines,
              "\$$tag using 1:2 with linespoints linewidth 1.1 pointtype 7 "
              . "pointsize 0.35 linecolor rgb \"$color\" notitle";
            push @keyentries,
              sprintf(
                        'keyentry with linespoints linewidth 1.1 pointtype 7 '
                      . 'pointsize 0.35 linecolor rgb "%s" title "%s"',
                      $color, gp_quote(short_label($key))
              );
            $idx++;
            next;
        }

        # Decaying min..max envelope; very light fill since ~20 series
        # overlap on the shared axis.
        push @bands,
          "\$$tag using 1:3:4 with filledcurves "
          . "fillcolor rgb \"$color\" fillstyle transparent solid 0.08 "
          . 'notitle';
        # Running max (slowest seen) and min (fastest seen) trace the band
        # edges, alpha-blended (#80 = 50% transparent) so they read lighter
        # than the solid rolling average.
        push @maxlines,
          "\$$tag using 1:4 with lines dashtype (2,1) linewidth 0.7 " . "linecolor rgb \"$light\" notitle";
        push @minlines,
          "\$$tag using 1:3 with lines dashtype (3,2) linewidth 0.7 " . "linecolor rgb \"$light\" notitle";
        push @avglines,
          "\$$tag using 1:5 with lines linewidth 1.1 " . "linecolor rgb \"$color\" notitle";
        push @points,
          "\$$tag using 1:2 with points pointtype 7 pointsize 0.25 " . "linecolor rgb \"$color\" notitle";
        push @keyentries,
          sprintf(
                    'keyentry with linespoints linewidth 1.1 pointtype 7 '
                  . 'pointsize 0.25 linecolor rgb "%s" title "%s"',
                  $color, gp_quote(short_label($key))
          );
        $idx++;
    }

    my @plots;
    if ($ratio) {
        # The parity line first, so the series stay legible over it; the
        # axis always shows it, whichever side of 1.00 the ratios sit.
        $data_min = 1 if $data_min > 1;
        $data_max = 1 if $data_max < 1;
        @plots = (
                  "1 with lines linewidth 2.4 linecolor rgb \"$PARITY_COLOR\" " . 'title "1.00 = parity"',
                  @avglines, @keyentries
        );
    } else {
        # Line-style legend entries explaining the three marks, appended
        # after the per-test colors.
        push @keyentries,
          'keyentry with lines linewidth 1.1 linecolor rgb "#333333" ' . 'title "avg (last 30 PRs)"',
          'keyentry with lines dashtype (2,1) linewidth 0.7 ' . 'linecolor rgb "#888888" title "max (decaying)"',
          'keyentry with lines dashtype (3,2) linewidth 0.7 ' . 'linecolor rgb "#888888" title "min (decaying)"';

        # Bottom-to-top: bands, then band-edge min/max traces, then the
        # solid rolling averages and the raw per-PR points on top.
        @plots = (@bands, @maxlines, @minlines, @avglines, @points, @keyentries);
    }

    # Log y-axis with the same explicit per-decade tic list as
    # plot_lantest.pl, so sub-decade values stay readable to a linear eye.
    # Floor: 0.1 ms for runtimes; a ratio may sit far below 1.
    my $y_floor = $ratio ? 0.001 : 0.1;
    my $y_lo    = $data_min / 1.6;
    $y_lo = $y_floor if $y_lo < $y_floor;
    my $y_hi = $data_max * 1.6;
    my @ytics;
    my $dec = 10**int((log($y_lo) / log(10)) - 1);
    while ($dec <= $y_hi) {
        for my $m (1 .. 9) {
            my $v = $dec * $m;
            next if $v < $y_lo || $v > $y_hi;
            if ($m == 1 || $m == 2 || $m == 5) {
                push @ytics, sprintf('"%g" %g', $v, $v);
            } else {
                push @ytics, sprintf('"" %g 1', $v);
            }
        }
        $dec *= 10;
    }
    # Ratios cluster around parity, where the decade scheme leaves 1 to 2
    # bare: tenths as minor tics, 1.5 labelled.
    if ($ratio) {
        for my $t (11 .. 19) {
            my $v = $t / 10;
            next if $v < $y_lo || $v > $y_hi;
            push @ytics, $t == 15 ? sprintf('"%g" %g', $v, $v) : sprintf('"" %g 1', $v);
        }
    }
    my $ytics = join(', ', @ytics);

    # One tick per PR, thinned to at most ~25 labels when the series grows
    # toward the 100-entry cap so the axis never crushes.
    my $step = int(($n + 24) / 25);
    $step = 1 if $step < 1;
    my @xtics;
    for my $i (0 .. $n - 1) {
        if ($i % $step == 0 || $i == $n - 1) {
            push @xtics, sprintf('"#%s" %d', gp_quote($prs->[$i]), $i);
        } else {
            push @xtics, sprintf('"" %d 1', $i);
        }
    }
    my $xtics = join(', ', @xtics);

    my @header;
    push @header, $subtitle if defined $subtitle && $subtitle ne '';
    push @header,
      sprintf(
              'Tests: %d · PRs: %d (#%s … #%s)',
              scalar(@keys), $n, $prs->[0], $prs->[-1]
      );
    my $header = gp_quote(join("\n", @header));

    my $suptitle = gp_quote($title || 'Netatalk AFP Lantest — per-test runtime trend across PRs');
    my $ylabel   = gp_quote($opts->{ylabel});
    my $png      = "$out_base.png";
    my $plots    = join(", \\\n     ", @plots);
    my $xmax     = $n - 1 + 0.4;

    my $gp_bin = $ENV{GNUPLOT_BIN} || 'gnuplot';
    open my $gp, '|-', $gp_bin or die "cannot run $gp_bin: $!\n";
    print $gp <<"EOF";
# fontscale/linewidth/pointscale 1.8 matches matplotlib's dpi=130 (130/72),
# so the point sizes below mirror the sibling plotters. DejaVu Sans is
# matplotlib's default; pangocairo falls back to system sans if it is absent.
set terminal pngcairo noenhanced size $WIDTH_PX,$HEIGHT_PX \\
    fontscale 1.8 linewidth 1.8 pointscale 1.8 \\
    background rgb "$BACKGROUND_COLOR" font "DejaVu Sans,7"
set output "@{[ gp_quote($png) ]}"

set border 15 linewidth 0.8 linecolor rgb "#333333"
set tics nomirror out scale 0.6

# Title block manually placed above the plot for a fixed title/subtitle gap.
set tmargin at screen 0.86
set bmargin at screen 0.085
# Right margin reserved for the per-test legend column, sized so the plot
# border sits just shy of the legend box. The left margin is pinned tight:
# the ylabel's rightward offset above frees the space gnuplot's auto
# margin would otherwise reserve for it.
set lmargin at screen 0.058
set rmargin at screen 0.868
set label 1 "$suptitle" at screen 0.45, screen 0.955 center \\
    font "DejaVu Sans:Bold,11"
set label 2 "$header" at screen 0.45, screen 0.905 center \\
    font "DejaVu Sans,5.5" textcolor rgb "#222222"

set xrange [-0.4:$xmax]
set xtics rotate by 16 right font ",6" ($xtics)
set xlabel "PR (chronological)"
set logscale y
set yrange [$y_lo:$y_hi]
set format y "%g"
set ytics ($ytics) font ",6"
# Pulled in toward the tick numbers (positive x offset = rightward).
set ylabel "$ylabel" offset 1.5,0
# Dotted grid; minor (per-decade log) lines fainter so they don't read heavy.
set grid xtics ytics mytics linetype 1 dashtype (1,2) linewidth 1 \\
    linecolor rgb "#BBBBBB", linetype 1 dashtype (1,2) linewidth 1 \\
    linecolor rgb "#DDDDDD"
# Legend in the reserved right margin, outside the plot area so it never
# overlaps a series; top-aligned with the plot's top border and flush with
# the canvas's right edge so no dead space remains beyond it.
set key at screen 0.998, screen 0.86 top right Left reverse samplen 1.5 \\
    width -1 spacing 1.1 font ",6" \\
    box linewidth 0.6 linecolor rgb "#CCCCCC" opaque fillcolor rgb "#F5F5F5"

$blocks
plot $plots
EOF
    close $gp or die "gnuplot failed (status $?)\n";
    print STDERR "Wrote $png\n";
    return 1;
}

# Select the series by key prefix and suffix, as ratios with
# --ratio-prefix, and plot them.
sub main {
    my $output   = 'perf-history';
    my $title    = undef;
    my $subtitle = undef;
    my $prefix   = 'lantest.';
    my $suffix   = '.mean_ms';
    my $ratio_prefix;
    my $ylabel;
    GetOptions(
               'o|output=s'     => \$output,
               'title=s'        => \$title,
               'subtitle=s'     => \$subtitle,
               'prefix=s'       => \$prefix,
               'suffix=s'       => \$suffix,
               'ratio-prefix=s' => \$ratio_prefix,
               'ylabel=s'       => \$ylabel,
      )
      or die "usage: plot_perf_history.pl HISTORY.yml [-o BASENAME] "
      . "[--title T] [--subtitle S] [--prefix P] [--suffix S] "
      . "[--ratio-prefix P] [--ylabel L]\n";
    my $input = shift @ARGV
      or die "missing input history YAML (perf_history.pl output)\n";
    $ylabel //=
      defined $ratio_prefix
      ? "Ratio: $ratio_prefix over $prefix"
      : 'Mean runtime (ms) — lower is faster';

    my $hist = load_history($input);
    my %series;
    for my $key (keys %$hist) {
        next if length $prefix && index($key, $prefix) != 0;
        next
          if length $suffix
          && (length $key < length $suffix
              || substr($key, -length $suffix) ne $suffix);
        if (!defined $ratio_prefix) {
            $series{$key} = $hist->{$key};
            next;
        }
        # The quotient of the two sides at each PR, keyed and labelled by
        # the PREFIX series; equal timestamps mark one merge, so one run,
        # and a zero is a time below the millisecond.
        my $num_key = $ratio_prefix . substr($key, length $prefix);
        my %num     = map { $_->{pr} => $_ } @{$hist->{$num_key} // []};
        my @both    = grep {
            my $n = $num{$_->{pr}};
            $n && ($n->{ts} // '') eq ($_->{ts} // '') && $n->{value} > 0 && $_->{value} > 0
        } @{$hist->{$key}};
        $series{$key} = [map { {pr => $_->{pr}, ts => $_->{ts}, value => $num{$_->{pr}}{value} / $_->{value}} } @both];
    }
    return plot(\%series, $output, $title, $subtitle, {ratio => defined $ratio_prefix, ylabel => $ylabel})
      ? 0
      : 1;
}

exit main();
