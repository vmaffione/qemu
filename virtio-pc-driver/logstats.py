#!/usr/bin/env python

import re
import sys
import argparse
import numpy


description = "Python script to compute mean and standard deviation"
epilog = "2016 Vincenzo Maffione"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('-d', '--data-file',
                       help = "Path to file containing data", type=str,
                       required = True)
argparser.add_argument('-t', '--num-trials',
                       help = "Number of samples for each point", type=int,
                       default = 10)

args = argparser.parse_args()

x = dict()

x['items'] = []
x['kicks'] = []
x['latency'] = []

fin = open(args.data_file)
while 1:
    line = fin.readline()
    if line == '':
        break

    m = re.search(r'(\d+) items/s (\d+) kicks/s (\d+) avg_batch (\d+) latency', line)
    if m == None:
        continue

    x['items'].append(int(m.group(1)))
    x['kicks'].append(int(m.group(2)))
    x['latency'].append(int(m.group(4)))

fin.close()

for name in sorted(x):
    mean = numpy.mean(x[name])
    stddev = numpy.std(x[name])
    print("%10s   %10.1f %10.1f" % (name, mean, stddev))

print("%10s   %10.1f" % ('batch', numpy.mean(x['items']) / numpy.mean(x['kicks'])))