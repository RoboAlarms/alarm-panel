# B028: TLS refused a server whose chain ends in a cross-signed root

**Status: fixed and proven on a bench panel 2026-10-05** (see "On the panel"). Found in a
bench panel's serial log during the B023 soak, not reported by a person. The certificate bundle
now trusts a chain through the root it holds when the server sends that root cross-signed by one it
no longer holds (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`).

## Symptom
A bench panel whose update source was set to a web site checked for updates every 10, 20, then 40
minutes and failed each time with "The update server can't be reached", though the site answers a
browser and its certificate is valid.

## Evidence
The panel's log at each check (P4 v1.3, firmware 0.7.0):

```
esp-x509-crt-bundle: No matching trusted root certificate found
esp-x509-crt-bundle: Failed to verify certificate
esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x3000
ota: connect: ESP_ERR_HTTP_CONNECT
ota: update check failed (The update server can't be reached): looking again in 10 min
```

The site's chain, as `openssl s_client -showcerts` sees it (OpenSSL: "Verification: OK"):

```
0 s:CN=<the site>                      i:Google Trust Services, CN=WE1
1 s:Google Trust Services, CN=WE1      i:Google Trust Services LLC, CN=GTS Root R4
2 s:Google Trust Services LLC, CN=GTS Root R4   i:GlobalSign nv-sa, CN=GlobalSign Root CA
```

ESP-IDF 6.1's full bundle (`cacrt_all.pem`, 145 authorities) holds GTS Root R4 but not GlobalSign
Root CA.

## Cause
The bundle is not a list of trusted roots that mbedTLS searches: it is a verify callback, called
for the certificate at the top of the chain the server sent, that looks up that certificate's
issuer. Here the top is GTS Root R4 cross-signed by GlobalSign Root CA, so it looked up GlobalSign
Root CA, found nothing and refused the handshake, although the bundle holds GTS Root R4, the issuer
of the certificate one step down. OpenSSL and browsers build the shorter path and stop at GTS Root
R4. The panel's TLS clients use the bundle (the update check, the MQTT link and the notifiers
when no certificate of their own is set, the Z-Wave device software service), so any server on
such a chain was refused.

The update page's words ("can't be reached") don't say that the certificate was the problem: the
update check reports every failed connection the same way. Telling the two apart means new words
on screen, so it waits for a proposal (BL-167).

## Fix
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` in `firmware/crowpanel_p4/sdkconfig.defaults`:
ESP-IDF's trusted-certificate callback offers mbedTLS the bundle's root for any issuer it is
looking for, so the chain ends at GTS Root R4. ESP-IDF says it costs about 700 bytes of heap (mbedTLS
allocates from PSRAM here). The paused 4" board (`firmware/e32r40t`) has the same bundle without the
option; it gets it when that board is taken up again.

## On the panel
On a bench panel (P4 v1.3, the dev image with the option), 2026-10-05: the update check against the
same site connected and read its page, and now fails only because that page is not a release list
("The release list could not be read"); no certificate error in the log. Before, every check ended
at "No matching trusted root certificate found".
