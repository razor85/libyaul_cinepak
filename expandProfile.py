import csv
import pathlib
import re
import subprocess
import sys

from pathlib import Path

def demangleSingleName(mangledName):
  """
  Demangles C++ names following basic Itanium ABI mangling rules.
  """
  if not mangledName.startswith('_Z'):
    return mangledName
    
  try:
    # Remove _Z prefix
    name = mangledName[2:]
    
    # Parse name length and name
    result = []
    i = 0
    while i < len(name):
      if name[i].isdigit():
        # Extract length
        lengthStr = ''
        while i < len(name) and name[i].isdigit():
          lengthStr += name[i]
          i += 1
        nameLength = int(lengthStr)
        
        # Extract name component
        if i + nameLength <= len(name):
          result.append(name[i:i + nameLength])
          i += nameLength
        else:
          return mangledName
      else:
        # Handle special characters and types
        typeMap = {
          'v': 'void',
          'i': 'int',
          'l': 'long',
          'f': 'float',
          'd': 'double',
          'c': 'char',
          'b': 'bool',
          'E': ''
        }
        
        if name[i] in typeMap:
          result.append(typeMap[name[i]])
        i += 1
    
    # Reconstruct demangled name
    if len(result) == 0:
      return mangledName
      
    demangled = '::'.join([x for x in result if x])
    return demangled
    
  except Exception:
    return mangledName

def demangleCppNames(inputStr):
  # Find and replace all mangled names
  words = inputStr.split()
  outputStr = ""
  for word in words:
    demangled = demangleSingleName(word)
    outputStr += f'{demangled} ' 
  return outputStr

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
with open('kronos_profiler.csv', 'r') as filePtr:
  csvFile = csv.reader(filePtr)
  for index, line in enumerate(csvFile):
    if index == 0:
      header = line
    else:
      lines.append(line)

# Sort lines by time
lines = sorted(lines, key = lambda x: int(x[1]), reverse=True)

header = [header[0], header[1], 'Percent', header[2], 'Description']
totalTimeMs = float(0)
for index, line in enumerate(lines):
  totalTimeMs += float(line[1])

for index, line in enumerate(lines):
  address = line[2]
  # The options are:
  #   @<file>                Read options from <file>
  #   -a --addresses         Show addresses
  #   -b --target=<bfdname>  Set the binary file format
  #   -e --exe=<executable>  Set the input file name (default is a.out)
  #   -i --inlines           Unwind inlined functions
  #   -j --section=<name>    Read section-relative offsets instead of addresses
  #   -p --pretty-print      Make the output easier to read for humans
  #   -s --basenames         Strip directory names
  #   -f --functions         Show function names
  #   -C --demangle[=style]  Demangle function names
  #   -R --recurse-limit     Enable a limit on recursion whilst demangling.  [Default]
  #   -r --no-recurse-limit  Disable a limit on recursion whilst demangling
  #   -h --help              Display this information
  #   -v --version           Display the program's version
  args = [addr2line, str(address), '-i', '-p', '-f', '-r', '-s', '-e', str(elf)]
  completed = subprocess.run(args, capture_output = True)
  output = completed.stdout.decode(sys.getfilesystemencoding())
  output = output.replace('\n', '')
  output = output.replace('\r', '')

  percent = "{:.2f}".format(float(line[1]) / totalTimeMs)
  originalAddress = line[2]

  # Reorder row to: Count/Time/Percent
  line[0], line[1], line[2] = line[0], line[1], percent
  line.append('0x{0:08X}'.format(int(originalAddress, 16)))
  line.append(demangleCppNames(output))

with open('performance.csv', 'w', newline='') as csvFile:
  writer = csv.writer(csvFile, delimiter=',', quotechar='"', quoting=csv.QUOTE_MINIMAL)
  writer.writerow(header)
  for line in lines:
    writer.writerow(line)
