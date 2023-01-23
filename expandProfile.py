import csv
import pathlib
import subprocess
import sys

from pathlib import Path

def findElf():
  build = Path('./build')
  if not build.exists():
    return None

  files = build.glob('*.elf')
  for file in files:
    return file

  return None

def getOptBase():
  if sys.platform == 'linux':
    return '/opt'
  else:
    return 'E:/Desenvolvimento/SegaSaturn/msys64/opt'

addr2line = Path(getOptBase()) / Path('tool-chains/sh2eb-elf/bin/sh2eb-elf-addr2line.exe')
elf = None

if len(sys.argv) >= 2:
  elf = sys.argv[1]
else:
  elf = findElf()
  if elf is not None:
    print('Found elf file {}, using it.'.format(elf))
    elf = elf.absolute().as_posix()
  else:
    print('Usage {} filename.elf'.format(sys.argv[0]))
    sys.exit(0)

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

header = [header[0], header[1], 'Time/Count', header[2], 'Description']
for index, line in enumerate(lines):
  address = line[2]
  args = [addr2line, str(address), '-i', '-p', '-f', '-e', str(elf)]
  completed = subprocess.run(args, capture_output = True)
  output = completed.stdout.decode(sys.getfilesystemencoding())
  output = output.replace('\n', '')
  output = output.replace('\r', '')

  timeMs = str(float(line[1]) / float(line[0]))
  originalAddress = line[2]

  # Reorder row to: Count/Time/TimeMs
  line[0], line[1], line[2] = line[0], line[1], timeMs
  line.append('0x{0:08X}'.format(int(originalAddress, 16)))
  line.append(output)

with open('performance.csv', 'w', newline='') as csvFile:
  writer = csv.writer(csvFile, delimiter=',', quotechar='"', quoting=csv.QUOTE_MINIMAL)
  writer.writerow(header)
  for line in lines:
    writer.writerow(line)