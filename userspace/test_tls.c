/* TLS over the real stack: open a TLS 1.2/1.3 connection to host:443, do the
 * handshake, send an HTTPS GET, print the status line and negotiated cipher.
 * Cert verification is off here — this proves the crypto + handshake + record
 * layer run on Ranix, not the PKI. Ad-hoc; needs a NIC + connectivity. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "example.com";
	int ret;

	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_entropy_context entropy;

	mbedtls_net_init(&net);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_entropy_init(&entropy);

	if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
	                                 (const unsigned char *)"ranix", 5)) != 0) {
		printf("TLS FAIL drbg_seed -0x%x\n", -ret); return 1;
	}
	if ((ret = mbedtls_net_connect(&net, host, "443", MBEDTLS_NET_PROTO_TCP)) != 0) {
		printf("TLS FAIL connect -0x%x\n", -ret); return 1;
	}
	printf("TLS connected to %s:443\n", host);

	if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
	                                       MBEDTLS_SSL_TRANSPORT_STREAM,
	                                       MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
		printf("TLS FAIL config -0x%x\n", -ret); return 1;
	}
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

	if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
		printf("TLS FAIL setup -0x%x\n", -ret); return 1;
	}
	mbedtls_ssl_set_hostname(&ssl, host);
	mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);

	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			printf("TLS FAIL handshake -0x%x\n", -ret); return 1;
		}
	}
	printf("TLS handshake OK: %s / %s\n",
	       mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));

	char req[256];
	int n = snprintf(req, sizeof(req),
	                 "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
	if ((ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req, n)) <= 0) {
		printf("TLS FAIL write -0x%x\n", -ret); return 1;
	}

	unsigned char buf[1024];
	ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf) - 1);
	if (ret <= 0) { printf("TLS FAIL read -0x%x\n", -ret); return 1; }
	buf[ret] = '\0';
	char *cr = strchr((char *)buf, '\r');
	if (cr) *cr = '\0';
	printf("TLS OK status: %s\n", buf);

	mbedtls_ssl_close_notify(&ssl);
	mbedtls_net_free(&net);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	return 0;
}
