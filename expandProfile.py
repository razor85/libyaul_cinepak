import csv
import subprocess
import sys

if len(sys.argv) < 2:
  print('Usage {} filename.elf'.format(sys.argv[0]))
  sys.exit(0)

addr2line = '/opt/tool-chains/sh2eb-elf/bin/sh2eb-elf-addr2line.exe'
elf = sys.argv[1]

header = []
lines = []
with open('yabause_performance.csv', 'r') as filePtr:
  csvFile = csv.reader(filePtr)
  for index, line in enumerate(csvFile):
    if index == 0:
      header = line
    else:
      lines.append(line)

# Sort lines by time
lines = sorted(lines, key = lambda x: int(x[1]), reverse=True)

header.append('Description')
for index, line in enumerate(lines):
  address = line[2]
  args = [addr2line, str(address), '-i', '-p', '-f', '-e', str(elf)]
  completed = subprocess.run(args, capture_output = True)
  output = completed.stdout.decode(sys.getfilesystemencoding())
  output = output.replace('\n', '')
  output = output.replace('\r', '')
  line.append(str(output))

with open('performance.csv', 'w', newline='') as csvFile:
  writer = csv.writer(csvFile, delimiter=',', quotechar='"', quoting=csv.QUOTE_MINIMAL)
  writer.writerow(header)
  for line in lines:
    writer.writerow(line)