#!/usr/bin/env python3
"""Plot CSV traces exported by gcc_network_simulation_test (requires matplotlib)."""
import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='Directory containing the eight simulation CSV files')
    parser.add_argument('--output', required=True, type=Path, help='Output image path, e.g. gcc-validation.png')
    args = parser.parse_args()
    scenarios = [
        ('fixed_500k', 'Fixed capacity: 500 kbit/s'),
        ('fixed_1m', 'Fixed capacity: 1 Mbit/s'),
        ('fixed_3m', 'Fixed capacity: 3 Mbit/s'),
        ('capacity_step', 'Capacity: 1 Mbit/s -> 300 kbit/s -> 1.5 Mbit/s'),
        ('random_loss_5pct', '1 Mbit/s, 5% random loss, +/-2 ms jitter'),
        ('rtt_200ms', '1 Mbit/s, base RTT: 200 ms'),
        ('feedback_loss_10pct', '1 Mbit/s, 10% reverse RTCP loss'),
        ('application_limited', '2 Mbit/s, 100 kbit/s application, ALR probing'),
    ]
    figure, axes = plt.subplots(4, 2, figsize=(15, 13), constrained_layout=True)
    for ax, (name, title) in zip(axes.flat, scenarios):
        with (args.directory / (name + '.csv')).open() as handle:
            rows = [{k: float(v) for k, v in row.items()} for row in csv.DictReader(handle)]
        time = [r['time_s'] for r in rows]
        ax.plot(time, [r['capacity_bps'] / 1000 for r in rows], color='#30343b', linestyle='--', label='Link capacity')
        ax.plot(time, [r['target_bps'] / 1000 for r in rows], color='#1967c6', linewidth=1.4, label='GCC target')
        ax.plot(time, [r['received_bps'] / 1000 for r in rows], color='#da7c16', alpha=0.8, linewidth=1, label='Received (1 s window)')
        if name == 'application_limited':
            ax.axhline(100, color='#a02847', linestyle=':', label='Application limit')
        if name == 'capacity_step':
            for at in [30, 60]:
                ax.axvline(at, color='#777777', linestyle=':', linewidth=0.8)
        ax.set(title=title, xlabel='Virtual time (s)', ylabel='Rate (kbit/s)', xlim=(0, time[-1]), ylim=(0, None))
        ax.grid(alpha=0.2)
        ax.legend(fontsize=8, loc='best')
    figure.suptitle('Closed-loop GCC validation | 1 ms virtual clock | Production GCC / Pacer / RTCP / TWCC', fontsize=14)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=150)
    plt.close(figure)
    print(args.output)


if __name__ == '__main__':
    main()
