"""Explicit, owner-authorized HIL: reject a derived client certificate and revoke
an existing session. Restores the ORIGINAL public client certificate via USB in
finally; never changes alarm codes or the firmware signing identity.
"""
import argparse
import datetime
import http.client
import re
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import panelctl as p
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.x509.oid import NameOID,ExtendedKeyUsageOID
import serial

def main():
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument("--device",required=True); a.add_argument("--port",required=True)
    args=a.parse_args(); c=p.Client(args.device); status=c.info()
    assert status["idle"] and not status["probation"]
    key_path=p.REGISTRY/c.device["key"]; cert_path=p.REGISTRY/c.device["cert"]
    cert=x509.load_pem_x509_certificate(cert_path.read_bytes())
    key=serialization.load_pem_private_key(key_path.read_bytes(),None)
    leaf=(x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,"unenrolled-derived-client")]))
      .issuer_name(cert.subject).public_key(key.public_key()).serial_number(x509.random_serial_number())
      .not_valid_before(datetime.datetime(2020,1,1)).not_valid_after(datetime.datetime(2070,1,1))
      .add_extension(x509.BasicConstraints(ca=False,path_length=None),True)
      .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]),False).sign(key,hashes.SHA256()))
    with tempfile.TemporaryDirectory(dir=p.ROOT/"build") as folder:
        path=Path(folder)/"derived.crt.pem"; path.write_bytes(leaf.public_bytes(serialization.Encoding.PEM))
        conn=p.PinnedConnection(c.host,c.device["port"],c.device["fingerprint"],path,key_path)
        try:
            conn.request("GET","/dev/v1/info"); response=conn.getresponse()
            assert response.status==403, "Derived certificate incorrectly gained developer authority"
        finally: conn.close()
    print("PASS TLS-valid derived client certificate rejected by exact enrollment fingerprint",flush=True)
    port=serial.Serial(port=None,baudrate=115200,timeout=.1)
    port.dtr=False; port.rts=False; port.port=args.port; port.open()
    def command(value,expect):
        port.reset_input_buffer(); port.write((value+"\r").encode()); buf=b""; until=time.monotonic()+10
        while time.monotonic()<until:
            buf+=port.read(4096)
            if re.search(expect,buf.decode("ascii","ignore")): return
        raise RuntimeError("Enrollment maintenance timed out: "+value.split()[1])
    try:
        # Reuse this already authenticated connection across revocation.
        c.info(); command("dev revoke",r"DEV REVOKE ESP_OK")
        try:
            c.info()
        except (OSError,RuntimeError,http.client.HTTPException):
            print("PASS established client session revoked",flush=True)
        else: raise AssertionError("Revoked client kept access")
    finally:
        try:
            command("dev begin",r"DEV BEGIN OK")
            der=cert.public_bytes(serialization.Encoding.DER)
            for at in range(0,len(der),80):
                end=min(at+80,len(der)); command("dev chunk "+der[at:end].hex(),rf"DEV CHUNK {end}\b")
            command("dev commit",r"DEV ENROLLED")
        finally: port.close(); c.close()
    time.sleep(3)
    assert c.info()["id"]==status["id"]
    print("PASS original developer enrollment restored and authenticated",flush=True)
    c.close()

if __name__=="__main__": main()
