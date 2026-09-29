"""Run partition safety checks and host-side tests of portable firmware logic."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(args):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str,args)), cwd=ROOT, check=True, timeout=120)

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--cpp-only',action='store_true')
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    run([sys.executable, '-B', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py'])
    compiler=os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise SystemExit('Install g++/clang++; Windows can run this script inside WSL.')
    with tempfile.TemporaryDirectory(prefix='rehab-tests-') as folder:
        for source in sorted((ROOT/'tests').glob('test_*.cpp')):
            target=Path(folder)/(source.stem+('.exe' if os.name=='nt' else ''))
            flags=['-std=c++17','-Wall','-Wextra','-Werror','-pedantic']
            if args.sanitize: flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            run([compiler,*flags,source,'-o',target])
            run([target])
    if not args.cpp_only:
        node=shutil.which('node')
        if not node: raise SystemExit('Install Node.js >=20 to run frontend tests, or use --cpp-only.')
        run([node,'--test','tests/frontend.test.cjs'])
    print('All requested host tests passed.')
