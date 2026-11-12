/*
 * Regression tests for WNM candidate selection and sleep key handling.
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

/* Include the production file to exercise its internal sleep handler too. */
#include WNM_REGRESSION_SOURCE

static unsigned int null_candidate_calls;
static unsigned int installed_keys;
static u8 installed_ids[2];

struct wpa_bss * __wrap_wpa_supplicant_select_bss(
	struct wpa_supplicant *wpa_s, struct wpa_ssid *group,
	struct wpa_ssid **selected_ssid, int only_first_ssid)
{
	/* A BTM candidate list with no matching scan result. */
	return NULL;
}

struct wpa_ssid * __wrap_wpa_scan_res_match(
	struct wpa_supplicant *wpa_s, int i, struct wpa_bss *bss,
	struct wpa_ssid *group, int only_first_ssid, int debug_print, bool link)
{
	return group;
}

int __wrap_wpa_supplicant_need_to_roam_within_ess(
	struct wpa_supplicant *wpa_s, struct wpa_bss *current_bss,
	struct wpa_bss *selected, bool poll_current)
{
	/* Report the violated call contract without dereferencing NULL. */
	if (!selected)
		null_candidate_calls++;
	return 1;
}

int __wrap_wpa_sm_pmf_enabled(struct wpa_sm *sm)
{
	return 1;
}

int __wrap_wpa_wnmsleep_install_key(struct wpa_sm *sm, u8 id, u8 *buf)
{
	/* Observe accepted key elements; do not install keys or touch a device. */
	if (installed_keys < ARRAY_SIZE(installed_ids))
		installed_ids[installed_keys] = id;
	installed_keys++;
	return 0;
}

static int check(int condition, const char *name)
{
	printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
	return !condition;
}

int main(void)
{
	struct wpa_supplicant wpa_s = { 0 };
	struct wpa_driver_ops driver = { 0 };
	struct wpa_config conf = { 0 };
	struct wpa_ssid ssid = { 0 };
	struct wpa_bss current_bss = { 0 };
	/* Three-byte response prefix, then two 32-byte management key elements. */
	u8 response[3 + 2 * (2 + 8 + 32)] = { 0 };
	u8 *igtk = response + 3;
	u8 *bigtk = igtk + 2 + 8 + 32;
	int failures = 0;

	wpa_debug_level = MSG_ERROR;
	wpa_s.driver = &driver;
	wpa_s.conf = &conf;
	wpa_s.current_ssid = &ssid;
	wpa_s.current_bss = &current_bss;
	wpa_s.wnm_dialog_token = 1;
	wpa_s.wnm_transition_scan = true;
	wnm_scan_process(&wpa_s, false);
	failures += check(null_candidate_calls == 0 &&
			  wpa_s.wnm_dialog_token == 0,
			  "BTM with no candidate skips roaming comparison and resets request");

	wpa_s.mgmt_group_cipher = WPA_CIPHER_BIP_GMAC_256;
	igtk[0] = WNM_SLEEP_SUBELEM_IGTK;
	igtk[1] = 8 + 32;
	bigtk[0] = WNM_SLEEP_SUBELEM_BIGTK;
	bigtk[1] = 8 + 32;
	wnm_sleep_mode_exit_success(&wpa_s, response, sizeof(response) - 3);
	failures += check(installed_keys == 2 &&
			  installed_ids[0] == WNM_SLEEP_SUBELEM_IGTK &&
			  installed_ids[1] == WNM_SLEEP_SUBELEM_BIGTK,
			  "WNM Sleep parses consecutive 32-byte IGTK and BIGTK");

	installed_keys = 0;
	igtk[1] = 8 + 16;
	wnm_sleep_mode_exit_success(&wpa_s, response, 2 + 8 + 16);
	failures += check(installed_keys == 0,
			  "WNM Sleep rejects a key shorter than the negotiated cipher");
	return failures ? 1 : 0;
}
