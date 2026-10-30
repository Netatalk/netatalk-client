#!/usr/bin/env perl

# Merge one CI run's performance metrics into the gh-pages history file and
# emit the dashboard's markdown fragments (current value vs. historical
# min/avg/max with a percent delta from the average). Only core Perl modules
# are used, so the script runs on a bare GitHub runner.
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

# Metrics arrive as pipe-separated lines, one metric per line:
#
#     render|key|display name|unit|value
#
# where render is "table" (a row in the section's markdown table) or
# "inline" (a bold run-on stat for the section header). Keys are free-form
# and self-describing (e.g. lantest.creating_2000_files.mean_ms); a renamed
# test simply starts a new key, and the old key is pruned automatically
# once it stops receiving entries, so nothing is positional and metrics
# can be added, renamed or dropped at any time.
#
# The history file is a YAML mapping of key -> list of {pr, ts, value}
# entries. Each PR holds at most one entry per key: a re-run of the same PR
# replaces its previous value (so a PR's repeated CI pushes don't skew the
# baseline), and each list is trimmed to the newest 100 entries. The
# historical stats for the delta exclude the current PR's own entry and
# cover only the newest 30 stored values, so the baseline follows recent
# performance instead of averaging the whole stored series.
#
# The YAML reader/writer below handles exactly the shape this script
# writes (block mapping of block sequences of flat mappings); anything it
# cannot parse is treated as no history for that key, mirroring the
# graceful degradation for hand-edited files.
#
# Usage:
#     perf_history.pl --history FILE --pr N --timestamp ISO8601 \
#         --section name=metrics_file [--section name=metrics_file ...] \
#         [--adjust name ...] [--adjust-exclude REGEX] \
#         [--ratio name=NUM/DEN[:LABEL] ...] [--ratio-exclude REGEX] \
#         [--github-output FILE]
#
# For every --section the script writes a "<name>_table" and "<name>_inline"
# output (empty string when there is no data) to --github-output (defaults
# to $GITHUB_OUTPUT), and rewrites --history in place.
#
# Sections named by --adjust gain an "Adj %" column: each metric's
# current/avg delta shifted by the section's runner baseline (the median
# delta across its table metrics, minus keys matching --adjust-exclude).
# The MAD of the deltas is reported alongside the baseline as a
# uniformity indicator. This cancels run-to-run runner-speed variance so
# genuine code effects stand out; standouts beyond 5% render bold.
#
# Sections named by --ratio compare two runs of the same workload, such as
# a mount under test and a control mount measured in the same CI run. NUM
# and DEN are key prefixes (the first dot-separated segment): the ratio of
# a metric "<prefix>.<rest>" is the current value of "NUM.<rest>" over
# that of "DEN.<rest>", both from the sections loaded in this run, and its
# history is the same quotient at every stored PR whose two values carry
# one timestamp, the mark of one merge, over the window of the other
# stats. A zero on either side, a time below the millisecond, gives no
# ratio. Table rows gain the current ratio, its delta from the historical
# average and that average with its min and max; "≈" marks a ratio with a
# side so small that its integer rounding alone moves it by over a tenth.
# Each inline stat is followed by a line of its own with the same figures,
# named after the stat with LABEL after its first word (Peak Read becomes
# Peak AFP/SMB Read). LABEL (default "NUM/DEN") heads the ratio column.
# Keys matching --ratio-exclude get no ratio: a dash in the table, no
# inline line; the per-operation time is the total over a constant count.
# Nothing is stored: the history keeps the two measured series only.

use strict;
use warnings;
use Encode         qw(decode);
use Getopt::Long   qw(GetOptions);
use File::Basename qw(dirname);
use File::Path     qw(make_path);

my $MAX_ENTRIES_PER_METRIC = 100;

# Historical stats window: hist_stats() averages only this many of a key's
# newest entries. Long enough to smooth runner variance, short enough that
# an accepted performance change becomes the new baseline within weeks
# rather than being diluted by the full 100-entry series.
my $HIST_STATS_WINDOW = 30;

# A key with no entry in its group's most recent runs (distinct PRs) is
# obsolete — its test was renamed or removed — and gets pruned. Three runs
# of grace keep a test that merely skipped a run or two alive.
my $PRUNE_GRACE_RUNS = 3;

# Parse pipe-separated metric lines; skip anything malformed.
sub load_metrics {
    my ($path) = @_;
    my @rows;
    open my $fh, '<:encoding(UTF-8)', $path or return \@rows;
    while (my $line = <$fh>) {
        $line =~ s/^\s+|\s+$//g;
        my @parts = map {s/^\s+|\s+$//gr} split /\|/, $line, -1;
        next unless @parts == 5;
        my ($render, $key, $name, $unit, $val) = @parts;
        next unless $render eq 'table' || $render eq 'inline';
        next unless length $key;
        next unless $val =~ /^-?\d+(?:\.\d+)?$/;
        push @rows, [$render, $key, $name, $unit, $val + 0];
    }
    close $fh;
    return \@rows;
}

# Read the history YAML: a mapping of key -> list of {pr, ts, value}.
# Unparseable keys or entries are dropped rather than fatal.
sub load_history {
    my ($path) = @_;
    my %hist;
    open my $fh, '<:encoding(UTF-8)', $path or return \%hist;
    my $key;
    my $entry;
    while (my $line = <$fh>) {
        chomp $line;
        next if $line =~ /^\s*(?:#.*)?$/;
        if ($line =~ /^(\S.*?):\s*$/) {
            # top-level key opening a block sequence
            $key        = _yaml_unquote($1);
            $entry      = undef;
            $hist{$key} = [];
        } elsif (defined $key && $line =~ /^- (\w+): (.*)$/) {
            # first field of a new sequence entry
            $entry = {};
            push @{$hist{$key}}, $entry;
            $entry->{$1} = _yaml_unquote($2);
        } elsif (defined $entry && $line =~ /^  (\w+): (.*)$/) {
            $entry->{$1} = _yaml_unquote($2);
        } else {
            # unrecognized shape (e.g. hand-edited scalar): drop the key
            delete $hist{$key} if defined $key;
            $key = $entry = undef;
        }
    }
    close $fh;
    # keep only well-formed entries
    for my $k (keys %hist) {
        $hist{$k} =
          [grep { defined $_->{pr} && defined $_->{value} && $_->{value} =~ /^-?\d+(?:\.\d+)?$/ } @{$hist{$k}}];
    }
    return \%hist;
}

# A YAML scalar without the quotes _yaml_quote may have added.
sub _yaml_unquote {
    my ($s) = @_;
    $s =~ s/^'(.*)'$/$1/;
    $s =~ s/^"(.*)"$/$1/;
    return $s;
}

# Quote a YAML scalar unless it is a plain number or a simple word.
sub _yaml_quote {
    my ($s) = @_;
    return $s if $s =~ /^-?\d+(?:\.\d+)?$/ || $s =~ /^[A-Za-z0-9_.\/-]+$/;
    $s =~ s/'/''/g;
    return "'$s'";
}

# Write the history in the shape load_history reads, keys without entries
# left out.
sub save_history {
    my ($path, $hist) = @_;
    my $dir = dirname($path);
    make_path($dir) if length $dir && !-d $dir;
    open my $fh, '>:encoding(UTF-8)', $path or die "cannot write $path: $!\n";
    for my $key (sort keys %$hist) {
        next unless @{$hist->{$key}};
        print $fh _yaml_quote($key) . ":\n";
        for my $e (@{$hist->{$key}}) {
            print $fh "- pr: $e->{pr}\n";
            print $fh "  ts: " . _yaml_quote($e->{ts} // '') . "\n";
            print $fh "  value: $e->{value}\n";
        }
    }
    close $fh;
}

# Integers plain; otherwise one decimal from 10 up, two below.
sub fmt_value {
    my ($v) = @_;
    return sprintf('%d', $v) if $v == int($v);
    return abs($v) >= 10 ? sprintf('%.1f', $v) : sprintf('%.2f', $v);
}

# Ratios keep two decimals so 1.00 reads as parity at a glance.
sub fmt_ratio {
    my ($r) = @_;
    return sprintf('%.2f', $r);
}

# The name of a stat's ratio line: the ratio label after the stat's first
# word, so "Peak Read" becomes "Peak AFP/SMB Read".
sub ratio_name {
    my ($name, $label) = @_;
    (my $r = $name) =~ s/^(\S+)/$1 $label/;
    return $r;
}

# Signed percent change from the average; a dash when the average is 0.
sub fmt_delta {
    my ($cur, $avg) = @_;
    return "\x{2014}" if $avg == 0;
    return sprintf('%+.1f%%', 100.0 * ($cur - $avg) / $avg);
}

# The parenthesised history note of an inline stat: delta from the
# average, the average with its unit, the min and max, the PR count.
sub fmt_hist_note {
    my ($fmt, $value, $unit, $avg, $min, $max, $n) = @_;
    my $prs = $n == 1 ? 'PR' : 'PRs';
    $unit = length $unit ? " $unit" : '';
    return
      sprintf(
              ' (%s vs hist avg %s%s; min %s / max %s over %d %s)',
              fmt_delta($value, $avg), $fmt->($avg), $unit,
              $fmt->($min), $fmt->($max), $n, $prs
      );
}

# (avg, min, max, count) of the newest $HIST_STATS_WINDOW of the given
# values, chronological order. Returns an empty list for no values.
sub window_stats {
    my @values = @_;
    splice @values, 0, @values - $HIST_STATS_WINDOW
      if @values > $HIST_STATS_WINDOW;
    return unless @values;
    my ($sum, $min, $max) = (0, $values[0], $values[0]);
    for my $v (@values) {
        $sum += $v;
        $min = $v if $v < $min;
        $max = $v if $v > $max;
    }
    return ($sum / @values, $min, $max, scalar @values);
}

# Window stats of a key's stored values, excluding the current PR's own
# entry.
sub hist_stats {
    my ($hist, $key, $current_pr) = @_;
    return
      window_stats(map  { $_->{value} + 0 }
                   grep { $_->{pr} ne $current_pr } @{$hist->{$key} // []});
}

# Window stats of the per-PR quotient of two keys' stored values over the
# PRs holding both from one merge, equal timestamps, so a rerun that lacked
# a side leaves that PR without a ratio rather than mixing runs; the
# current PR's own entries are excluded.
sub ratio_hist_stats {
    my ($hist, $num_key, $den_key, $current_pr) = @_;
    my %num = map { $_->{pr} => $_ } @{$hist->{$num_key} // []};
    my @values;
    for my $e (@{$hist->{$den_key} // []}) {
        next if $e->{pr} eq $current_pr;
        my $n = $num{$e->{pr}};
        next unless $n && ($n->{ts} // '') eq ($e->{ts} // '') && $n->{value} > 0 && $e->{value} > 0;
        push @values, $n->{value} / $e->{value};
    }
    return window_stats(@values);
}

# The two keys a ratio of the given key divides: the key's first segment
# swapped for the ratio's numerator and denominator prefixes.
sub ratio_keys {
    my ($ratio, $key) = @_;
    (my $rest = $key) =~ s/^[^.]*\.//;
    return ("$ratio->{num}.$rest", "$ratio->{den}.$rest");
}

# Current ratio of a key from this run's rows as (ratio, coarse): an empty
# list when a side is missing or zero; coarse when a side is so small
# that its integer rounding alone moves the ratio by more than a tenth.
sub current_ratio {
    my ($ratio, $current, $key) = @_;
    my ($num_key, $den_key) = ratio_keys($ratio, $key);
    my $num = $current->{$num_key};
    my $den = $current->{$den_key};
    return unless defined $num && defined $den && $num > 0 && $den > 0;
    return ($num / $den, 0.5 / $num + 0.5 / $den > 0.1);
}

# Parse "SECTION=NUM/DEN[:LABEL]" ratio specs into section => spec.
sub parse_ratios {
    my (@specs) = @_;
    my %ratio;
    for my $spec (@specs) {
        # Decoded like the metrics files, so a non-ASCII label survives the
        # UTF-8 output layer.
        my ($section, $num, $den, $label) = decode('UTF-8', $spec) =~ m{^([^=]+)=([^/:]+)/([^/:]+)(?::(.+))?$}
          or die "bad --ratio (want SECTION=NUM/DEN[:LABEL]): $spec\n";
        $ratio{$section} = {num => $num, den => $den, label => $label // "$num/$den"};
    }
    return \%ratio;
}

# Replace or append the PR's entry for a key, keeping the newest
# $MAX_ENTRIES_PER_METRIC.
sub upsert {
    my ($hist, $key, $pr, $ts, $value) = @_;
    my @entries = grep { $_->{pr} ne $pr } @{$hist->{$key} // []};
    push @entries, {pr => $pr, ts => $ts, value => $value};
    splice @entries, 0, @entries - $MAX_ENTRIES_PER_METRIC
      if @entries > $MAX_ENTRIES_PER_METRIC;
    $hist->{$key} = \@entries;
}

# Prune obsolete keys after the current run's rows have been upserted.
# When a test is renamed or removed, its old key stops receiving entries
# while the rest of its group (keys sharing the first dot-separated
# segment, e.g. "lantest") moves on; without pruning the dead key stays in
# the history — and in every future trend-chart legend — forever, since
# the per-key entry cap only trims keys that still receive data. A key
# with no entry in its group's $PRUNE_GRACE_RUNS most recent runs
# (distinct PRs, ranked by their newest timestamp) is dropped. Only groups
# with rows in the current run are examined: a section whose CI job failed
# contributes no rows, and its keys must not age toward pruning on another
# section's clock.
sub prune_stale {
    my ($hist, $rows) = @_;
    my %groups = map { ((split /\./, $_->[1], 2)[0]) => 1 } @$rows;
    for my $group (keys %groups) {
        my @keys = grep { (split /\./, $_, 2)[0] eq $group } keys %$hist;
        my %pr_ts;
        for my $key (@keys) {
            for my $e (@{$hist->{$key}}) {
                my $ts = $e->{ts} // '';
                $pr_ts{$e->{pr}} = $ts
                  if !exists $pr_ts{$e->{pr}} || $ts gt $pr_ts{$e->{pr}};
            }
        }
        my @recent =
          (sort { $pr_ts{$b} cmp $pr_ts{$a} } keys %pr_ts)[0 .. $PRUNE_GRACE_RUNS - 1];
        my %recent = map { $_ => 1 } grep {defined} @recent;
        for my $key (@keys) {
            delete $hist->{$key}
              unless grep { $recent{$_->{pr}} } @{$hist->{$key}};
        }
    }
}

# Median of a list, undef for an empty one.
sub median {
    my @sorted = sort { $a <=> $b } @_;
    return unless @sorted;
    my $mid = int(@sorted / 2);
    return @sorted % 2 ? $sorted[$mid] : ($sorted[$mid - 1] + $sorted[$mid]) / 2;
}

# Runner-speed baseline for an adjusted section: the median of the
# current/avg deltas across its history-backed table metrics, plus the MAD
# (median absolute deviation) of those deltas.  All tests in one run move
# together with the runner's speed, and the median ignores the few a PR
# genuinely changed, so shifting each delta by it isolates code effects
# from runner variance; the MAD shows how uniformly the run actually moved
# (a large MAD means the shift is not common-mode and the adjusted column
# should be read with suspicion).  Keys matching the exclude pattern
# (e.g. the IO-bound streaming tests, whose variance does not track the
# CPU-bound op tests) contribute nothing and get no adjusted value.
sub baseline_delta {
    my ($rows, $hist, $pr, $exclude_re) = @_;
    my @deltas;
    for my $row (@$rows) {
        my ($render, $key, $name, $unit, $value) = @$row;
        next unless $render eq 'table';
        next if defined $exclude_re && $key =~ /$exclude_re/;
        my ($avg) = hist_stats($hist, $key, $pr);
        push @deltas, 100.0 * ($value / $avg - 1) if defined $avg && $avg > 0;
    }
    my $med = median(@deltas);
    return unless defined $med;
    my $mad = median(map { abs($_ - $med) } @deltas);
    return ($med, $mad);
}

# Build the (table_md, inline_md) fragments for one section. $ratio is the
# section's --ratio spec or undef; $current maps every key of this run,
# across all sections, to its value.
sub render_section {
    my ($rows, $hist, $pr, $adjust, $exclude_re, $ratio, $ratio_exclude, $current) = @_;
    my (@table_rows, @inline_bits, $any_coarse);
    my $unit_hdr = '';
    my ($base_d, $base_mad);
    ($base_d, $base_mad) = baseline_delta($rows, $hist, $pr, $exclude_re)
      if $adjust;
    for my $row (@$rows) {
        my ($render, $key, $name, $unit, $value) = @$row;
        my ($avg, $min, $max, $n) = hist_stats($hist, $key, $pr);
        my $r      = $ratio && (!defined $ratio_exclude || $key !~ /$ratio_exclude/) ? $ratio : undef;
        my @r_hist = $r ? ratio_hist_stats($hist, ratio_keys($r, $key), $pr)                  : ();
        my ($r_cur, $r_coarse) = $r ? current_ratio($r, $current, $key) : ();
        my $r_mark = $r_coarse ? "\x{2248}" : '';
        $any_coarse ||= $r_coarse;
        if ($render eq 'inline') {
            my $stat = sprintf('**%s: %s %s**', $name, fmt_value($value), $unit);
            $stat .= fmt_hist_note(\&fmt_value, $value, $unit, $avg, $min, $max, $n)
              if defined $avg;
            push @inline_bits, $stat;
            if (defined $r_cur) {
                my $r_stat = sprintf('**%s: %s%s**', ratio_name($name, $ratio->{label}), $r_mark, fmt_ratio($r_cur));
                $r_stat .= fmt_hist_note(\&fmt_ratio, $r_cur, '', @r_hist)
                  if @r_hist;
                push @inline_bits, $r_stat;
            }
        } else {
            $unit_hdr ||= $unit;
            my @cells =
              defined $avg
              ? (fmt_delta($value, $avg), fmt_value($avg), fmt_value($min), fmt_value($max))
              : ("\x{2014}") x 4;
            if ($adjust) {
                my $adj = "\x{2014}";
                if (    defined $base_d
                     && defined $avg
                     && $avg > 0
                     && (!defined $exclude_re || $key !~ /$exclude_re/)) {
                    my $pct = 100.0 * ($value / $avg - 1) - $base_d;
                    $adj = sprintf('%+.1f%%', $pct);
                    # standout beyond the run's baseline: a code effect,
                    # not a runner property
                    $adj = "**$adj**" if abs($pct) >= 5;
                }
                splice @cells, 1, 0, $adj;
            }
            if ($ratio) {
                my ($r_avg, $r_min, $r_max) = @r_hist;
                push @cells, defined $r_cur ? $r_mark . fmt_ratio($r_cur) : "\x{2014}";
                push @cells,
                  defined $r_cur && defined $r_avg
                  ? fmt_delta($r_cur, $r_avg)
                  : "\x{2014}";
                push @cells,
                  defined $r_avg
                  ? (fmt_ratio($r_avg), fmt_ratio($r_min), fmt_ratio($r_max))
                  : ("\x{2014}") x 3;
            }
            push @table_rows,
              sprintf(
                      '| %s | %s | ' . join(' | ', ('%s') x @cells) . ' |',
                      $name, fmt_value($value), @cells
              );
        }
    }
    my $table_md = '';
    if (@table_rows) {
        my $adj_hdr   = $adjust ? "Adj \x{394}% | " : '';
        my $adj_sep   = $adjust ? "-------|"        : '';
        my $ratio_hdr = '';
        my $ratio_sep = '';
        if ($ratio) {
            $ratio_hdr = " $ratio->{label} | Ratio \x{394}% | Ratio hist avg | Ratio hist min | Ratio hist max |";
            $ratio_sep = "-------|------------|----------------|----------------|----------------|";
        }
        $table_md =
            "| Metric | Current ($unit_hdr) | Cur Avg \x{394}% | ${adj_hdr}Hist avg "
          . "| Hist min | Hist max |${ratio_hdr}\n"
          . "|--------|------------------|------------|${adj_sep}----------"
          . "|----------|----------|${ratio_sep}\n"
          . join("\n", @table_rows);

        if (defined $base_d) {
            $table_md .= sprintf(
                                   "\n\n_Run baseline: median op-test delta %+.1f%%, MAD %.1f%%. "
                                 . "Adj \x{394}%% shifts each delta by the median; standouts \x{2265}5%% in bold. "
                                 . "A large MAD means the run did not move uniformly \x{2014} "
                                 . "read the adjusted column with caution._",
                                 $base_d, $base_mad
            );
        }
        $table_md .=
            "\n\n_\x{2248}: one side of the ratio is so small that its integer rounding "
          . "alone moves the ratio by more than 10%._"
          if $any_coarse;
    }
    # One stat per line: markdown needs the trailing double-space for a
    # line break inside the same paragraph.
    return ($table_md, join("  \n", @inline_bits));
}

# One GitHub Actions output as a heredoc, so a multi-line value survives.
sub write_output {
    my ($fh, $name, $value) = @_;
    print $fh "$name<<EOF_$name\n$value\nEOF_$name\n";
}

# Render every section against the stored history, then merge the run
# into it.
sub main {
    my ($history_path, $pr, $timestamp);
    my @sections;
    my $github_output = $ENV{GITHUB_OUTPUT} // '/dev/null';
    my @adjust_sections;
    my $adjust_exclude;
    my @ratio_specs;
    my $ratio_exclude;
    GetOptions(
               'history=s'        => \$history_path,
               'pr=i'             => \$pr,
               'timestamp=s'      => \$timestamp,
               'section=s'        => \@sections,
               'adjust=s'         => \@adjust_sections,
               'adjust-exclude=s' => \$adjust_exclude,
               'ratio=s'          => \@ratio_specs,
               'ratio-exclude=s'  => \$ratio_exclude,
               'github-output=s'  => \$github_output,
      )
      or die "usage: perf_history.pl --history FILE --pr N --timestamp TS "
      . "--section NAME=FILE ... [--adjust NAME ...] "
      . "[--adjust-exclude REGEX] [--ratio NAME=NUM/DEN[:LABEL] ...] "
      . "[--ratio-exclude REGEX] [--github-output FILE]\n";
    die "missing --history/--pr/--timestamp\n"
      unless defined $history_path && defined $pr && defined $timestamp;
    my %adjust = map { $_ => 1 } @adjust_sections;
    my $ratio  = parse_ratios(@ratio_specs);

    my $hist = load_history($history_path);

    # Every section's rows are loaded before any is rendered: a ratio
    # divides by a value from another section of the same run.
    my (@loaded, %current);
    for my $spec (@sections) {
        my ($name, $path) = split /=/, $spec, 2;
        die "bad --section (want NAME=FILE): $spec\n" unless defined $path && length $path;
        my $rows = load_metrics($path);
        push @loaded, [$name, $rows];
        $current{$_->[1]} = $_->[4] for @$rows;
    }

    open my $out, '>>:encoding(UTF-8)', $github_output
      or die "cannot append $github_output: $!\n";
    my @all_rows;
    for my $section (@loaded) {
        my ($name, $rows) = @$section;
        my ($table_md, $inline_md) =
          render_section($rows, $hist, $pr, $adjust{$name}, $adjust_exclude, $ratio->{$name}, $ratio_exclude,
                         \%current);
        write_output($out, "${name}_table",  $table_md);
        write_output($out, "${name}_inline", $inline_md);
        upsert($hist, $_->[1], $pr, $timestamp, $_->[4]) for @$rows;
        push @all_rows, @$rows;
    }
    close $out;

    prune_stale($hist, \@all_rows);
    save_history($history_path, $hist);
    return 0;
}

exit main();
