"""Authenticate regression tests through the same login path as users."""
import sys
import tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from setup_security import provision
_TEMP=tempfile.TemporaryDirectory(prefix='osh-tests-')
STATE=provision(Path(_TEMP.name)/'state',{'admin':'AdminTest123!','student':'StudentTest123!','guest':'GuestTest123!'})
NESTED=provision(Path(_TEMP.name)/'nested',{'admin':'AdminTest123!','student':'StudentTest123!','guest':'GuestTest123!'})
AUTH='admin\nAdminTest123!\n'

def output(text):
    return text.split('Authenticated user=admin role=admin\n',1)[-1]
