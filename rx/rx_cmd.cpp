/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2014-2016 John Seamons, ZL/KF6VO

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "clk.h"
#include "misc.h"
#include "str.h"
#include "printf.h"
#include "timer.h"
#include "web.h"
#include "peri.h"
#include "spi.h"
#include "gps.h"
#include "cfg.h"
#include "dx.h"
#include "coroutines.h"
#include "data_pump.h"
#include "ext_int.h"
#include "net.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <math.h>
#include <signal.h>
#include <fftw3.h>

char *cpu_stats_buf;
volatile float audio_kbps, waterfall_kbps, waterfall_fps[RX_CHANS+1], http_kbps;
volatile int audio_bytes, waterfall_bytes, waterfall_frames[RX_CHANS+1], http_bytes;
char *current_authkey;
int debug_v;
bool auth_su;

//#define FORCE_ADMIN_PWD_CHECK

bool rx_common_cmd(const char *stream_name, conn_t *conn, char *cmd)
{
	int i, j, n;
	struct mg_connection *mc = conn->mc;
	char *sb, *sb2;
	int slen;
	
	if (mc == NULL) return false;
	
	NextTask("rx_common_cmd");      // breakup long runs of sequential commands -- sometimes happens at startup
	
	// SECURITY: auth command here is the only one allowed before auth check below
	if (kiwi_str_begins_with(cmd, "SET auth")) {
	
		const char *pwd_s = NULL;
		int cfg_auto_login;

		char *type_m = NULL, *pwd_m = NULL;
		n = sscanf(cmd, "SET auth t=%16ms p=%256ms", &type_m, &pwd_m);
		//cprintf(conn, "n=%d typem=%s pwd=%s\n", n, type_m, pwd_m);
		if ((n != 1 && n != 2) || type_m == NULL) {
			send_msg(conn, false, "MSG badp=1");
            free(type_m); free(pwd_m);      // free(NULL) is okay
			return true;
		}
		
		kiwi_str_decode_inplace(pwd_m);
		//printf("PWD %s pwd %d \"%s\" from %s\n", type_m, slen, pwd_m, conn->remote_ip);
		
		bool allow = false, cant_determine = false;
		bool type_kiwi = (type_m != NULL && strcmp(type_m, "kiwi") == 0);
		bool type_admin = (type_m != NULL && strcmp(type_m, "admin") == 0);
		
		bool stream_snd_or_wf = (conn->type == STREAM_SOUND || conn->type == STREAM_WATERFALL);
		bool stream_admin_or_mfg = (conn->type == STREAM_ADMIN || conn->type == STREAM_MFG);
		bool stream_ext = (conn->type == STREAM_EXT);
		
		bool bad_type = (stream_snd_or_wf || stream_ext || stream_admin_or_mfg)? false : true;
		
		if ((!type_kiwi && !type_admin) || bad_type) {
			clprintf(conn, "PWD BAD REQ type_m=\"%s\" conn_type=%d from %s\n", type_m, conn->type, conn->remote_ip);
			send_msg(conn, false, "MSG badp=1");
            free(type_m); free(pwd_m);
			return true;
		}
		
		// opened admin/mfg url, but then tried type kiwi auth!
		if (stream_admin_or_mfg && !type_admin) {
			clprintf(conn, "PWD BAD TYPE MIX type_m=\"%s\" conn_type=%d from %s\n", type_m, conn->type, conn->remote_ip);
			send_msg(conn, false, "MSG badp=1");
            free(type_m); free(pwd_m);
			return true;
		}
		
		bool log_auth_attempt = (stream_admin_or_mfg || (stream_ext && type_admin));
		isLocal_t isLocal = isLocal_IP(conn, log_auth_attempt);
		bool is_local = (isLocal == IS_LOCAL);

		#ifdef FORCE_ADMIN_PWD_CHECK
			is_local = false;
		#endif
		
		//cprintf(conn, "PWD %s log_auth_attempt %d conn_type %d [%s] isLocal %d is_local %d from %s\n",
		//	type_m, log_auth_attempt, conn->type, streams[conn->type].uri, isLocal, is_local, conn->remote_ip);
		
	    // Let client know who we think they are.
		// Use public ip of Kiwi server when client connection is on local subnet.
		// This distinction is for the benefit of setting the user's geolocation at short-wave.info
		char *client_public_ip = is_local? ddns.ip_pub : conn->remote_ip;
        send_msg(conn, false, "MSG client_public_ip=%s", client_public_ip);
        cprintf(conn, "client_public_ip %s\n", client_public_ip);

		int chan_no_pwd = cfg_int("chan_no_pwd", NULL, CFG_REQUIRED);
		int chan_need_pwd = RX_CHANS - chan_no_pwd;

		if (type_kiwi) {
			pwd_s = admcfg_string("user_password", NULL, CFG_REQUIRED);
			bool no_pwd = (pwd_s == NULL || *pwd_s == '\0');
			cfg_auto_login = admcfg_bool("user_auto_login", NULL, CFG_REQUIRED);
			
			// if no user password set allow unrestricted connection
			if (no_pwd) {
				cprintf(conn, "PWD kiwi ALLOWED: no config pwd set, allow any\n");
				allow = true;
			} else
			
			// config pwd set, but auto_login for local subnet is true
			if (cfg_auto_login && is_local) {
				cprintf(conn, "PWD kiwi ALLOWED: config pwd set, but is_local and auto-login set\n");
				allow = true;
			} else {
			
				int rx_free = rx_chan_free(NULL);
				
				// allow with no password if minimum number of channels needing password remains
				// if no password has been set at all we've already allowed access above
				if (rx_free >= chan_need_pwd) {
					allow = true;
					//cprintf(conn, "PWD rx_free=%d >= chan_need_pwd=%d %s\n", rx_free, chan_need_pwd, allow? "TRUE":"FALSE");
				}
			}
		} else
		
		if (type_admin) {
			pwd_s = admcfg_string("admin_password", NULL, CFG_REQUIRED);
			bool no_pwd = (pwd_s == NULL || *pwd_s == '\0');
			cfg_auto_login = admcfg_bool("admin_auto_login", NULL, CFG_REQUIRED);
			clprintf(conn, "PWD %s: config pwd set %s, auto-login %s\n", type_m,
				no_pwd? "FALSE":"TRUE", cfg_auto_login? "TRUE":"FALSE");

			// can't determine local network status (yet)
			if (no_pwd && isLocal == NO_LOCAL_IF) {
				clprintf(conn, "PWD %s CAN'T DETERMINE: no local network interface information\n", type_m);
				cant_determine = true;
			} else

			// no config pwd set (e.g. initial setup) -- allow if connection is from local network
			if (no_pwd && is_local) {
				clprintf(conn, "PWD %s ALLOWED: no config pwd set, but is_local\n", type_m);
				allow = true;
			} else
			
			// config pwd set, but auto_login for local subnet is true
			if (cfg_auto_login && is_local) {
				clprintf(conn, "PWD %s ALLOWED: config pwd set, but is_local and auto-login set\n", type_m);
				allow = true;
			} else
			
			#if 0
			// allow people to demo admin mode at kiwisdr.jks.com without changing actual admin configuration
			if (isLocal != NO_LOCAL_IF && ip_match(ddns.ip_pub, &ddns.ips_kiwisdr_com)) {
				clprintf(conn, "PWD %s: allowing admin demo mode on %s\n", type_m, ddns.ip_pub);
				conn->admin_demo_mode = true;
				allow = true;
			}
			#else
			{}
			#endif
		} else {
			cprintf(conn, "PWD bad type=%s\n", type_m);
			pwd_s = NULL;
		}
		
		#ifndef FORCE_ADMIN_PWD_CHECK
		    // can't allow based on ip address since it can now be spoofed via X-Real-IP and X-Forwarded-For
		    /*
			if (!allow && ip_match(conn->remote_ip, &ddns.ips_kiwisdr_com)) {
			    printf("PWD %s ALLOWED: by ip match\n", type_m);
				allow = true;
			}
			*/
			
			if (auth_su) {
			    printf("PWD %s ALLOWED: by su\n", type_m);
				allow = true;
			    auth_su = false;        // be certain to reset the global immediately
			}
		#endif
		
		int badp = 1;

		if (cant_determine) {
		    badp = 2;
		} else

		if (allow) {
			if (log_auth_attempt)
				clprintf(conn, "PWD %s ALLOWED: from %s\n", type_m, conn->remote_ip);
			badp = 0;
		} else
		
		if ((pwd_s == NULL || *pwd_s == '\0')) {
			clprintf(conn, "PWD %s REJECTED: no config pwd set, from %s\n", type_m, conn->remote_ip);
			badp = 1;
		} else {
			if (pwd_m == NULL || pwd_s == NULL)
				badp = 1;
			else {
				//cprintf(conn, "PWD CMP %s pwd_s \"%s\" pwd_m \"%s\" from %s\n", type_m, pwd_s, pwd_m, conn->remote_ip);
				badp = strcasecmp(pwd_m, pwd_s)? 1:0;
			}
			//clprintf(conn, "PWD %s %s: sent from %s\n", type_m, badp? "rejected":"accepted", conn->remote_ip);
		}
		
		send_msg(conn, false, "MSG rx_chans=%d", RX_CHANS);
		send_msg(conn, false, "MSG chan_no_pwd=%d", chan_no_pwd);
		send_msg(conn, false, "MSG badp=%d", badp);

        free(type_m); free(pwd_m);
		cfg_string_free(pwd_s);
		
		// only when the auth validates do we setup the handler
		if (badp == 0) {
		
		    // It's possible for both to be set e.g. auth_kiwi set on initial user connection
		    // then correct admin pwd given later for label edit.
		    
			if (type_kiwi || (type_admin && conn->admin_demo_mode)) conn->auth_kiwi = true;

			if (type_admin && !conn->admin_demo_mode) {
			    conn->auth_admin = true;
			    int chan = conn->rx_channel;

                // give admin auth to all associated conns
			    if (!stream_admin_or_mfg && chan != -1) {
                    conn_t *c = conns;
                    for (int i=0; i < N_CONNS; i++, c++) {
                        if (!c->valid || !(c->type == STREAM_SOUND || c->type == STREAM_WATERFALL || c->type == STREAM_EXT))
                            continue;
                        if (c->rx_channel == chan || (c->type == STREAM_EXT && c->ext_rx_chan == chan)) {
                            c->auth_admin = true;
                        }
                    }
			    }
			}

			if (conn->auth == false) {
				conn->auth = true;
				conn->isLocal = is_local;
				
				// send cfg once to javascript
				if (conn->type == STREAM_SOUND || stream_admin_or_mfg)
					rx_server_send_config(conn);
				
				// setup stream task first time it's authenticated
				stream_t *st = &streams[conn->type];
				if (st->setup) (st->setup)((void *) conn);
			}
		}

		return true;
	}

	if (strcmp(cmd, "SET keepalive") == 0) {
		conn->keepalive_count++;
		return true;
	}

	// SECURITY: we accept no incoming commands besides auth and keepalive until auth is successful
	if (conn->auth == false) {
		clprintf(conn, "### SECURITY: NO AUTH YET: %s %d %s <%s>\n", stream_name, conn->type, conn->remote_ip, cmd);
		return true;	// fake that we accepted command so it won't be further processed
	}

	if (strcmp(cmd, "SET is_admin") == 0) {
	    assert(conn->auth == true);
		send_msg(conn, false, "MSG is_admin=%d", conn->auth_admin);
		return true;
	}

	if (strcmp(cmd, "SET get_authkey") == 0) {
		if (conn->auth_admin == false) {
			cprintf(conn, "get_authkey NO ADMIN AUTH %s\n", conn->remote_ip);
			return true;
		}
		
		free(current_authkey);
		current_authkey = kiwi_authkey();
		send_msg(conn, false, "MSG authkey_cb=%s", current_authkey);
		return true;
	}

	if (kiwi_str_begins_with(cmd, "SET save_cfg=")) {
		if (conn->auth_admin == FALSE) {
			lprintf("** attempt to save kiwi config with auth_admin == FALSE! IP %s\n", conn->remote_ip);
			return true;	// fake that we accepted command so it won't be further processed
		}

		char *json = cfg_realloc_json(strlen(cmd), CFG_NONE);	// a little bigger than necessary
		n = sscanf(cmd, "SET save_cfg=%s", json);
		assert(n == 1);
		//printf("SET save_cfg=...\n");
		kiwi_str_decode_inplace(json);
		cfg_save_json(json);
		update_vars_from_config();      // update C copies of vars

		return true;
	}

	if (kiwi_str_begins_with(cmd, "SET save_adm=")) {
		if (conn->type != STREAM_ADMIN) {
			lprintf("** attempt to save admin config from non-STREAM_ADMIN! IP %s\n", conn->remote_ip);
			return true;	// fake that we accepted command so it won't be further processed
		}
	
		if (conn->auth_admin == FALSE) {
			lprintf("** attempt to save admin config with auth_admin == FALSE! IP %s\n", conn->remote_ip);
			return true;	// fake that we accepted command so it won't be further processed
		}

		char *json = admcfg_realloc_json(strlen(cmd), CFG_NONE);	// a little bigger than necessary
		n = sscanf(cmd, "SET save_adm=%s", json);
		assert(n == 1);
		//printf("SET save_adm=...\n");
		kiwi_str_decode_inplace(json);
		admcfg_save_json(json);
		//update_vars_from_config();    // no admin vars need to be updated on save currently
		
		return true;
	}

	if (strcmp(cmd, "SET GET_USERS") == 0) {
		rx_chan_t *rx;
		bool need_comma = false;
		sb = (char *) "[";
		bool isAdmin = (conn->type == STREAM_ADMIN);
		
		for (rx = rx_channels, i=0; rx < &rx_channels[RX_CHANS]; rx++, i++) {
			n = 0;
			if (rx->busy) {
				conn_t *c = rx->conn_snd;
				if (c && c->valid && c->arrived && c->user != NULL) {
					assert(c->type == STREAM_SOUND);
					u4_t now = timer_sec();
					u4_t t = now - c->arrival;
					u4_t sec = t % 60; t /= 60;
					u4_t min = t % 60; t /= 60;
					u4_t hr = t;
					char *user = c->isUserIP? NULL : kiwi_str_encode(c->user);
					char *geo = c->geo? kiwi_str_encode(c->geo) : NULL;
					char *ext = ext_users[i].ext? kiwi_str_encode((char *) ext_users[i].ext->name) : NULL;
					const char *ip = isAdmin? c->remote_ip : "";
					asprintf(&sb2, "%s{\"i\":%d,\"n\":\"%s\",\"g\":\"%s\",\"f\":%d,\"m\":\"%s\",\"z\":%d,\"t\":\"%d:%02d:%02d\",\"e\":\"%s\",\"a\":\"%s\"}",
						need_comma? ",":"", i, user? user:"", geo? geo:"", c->freqHz,
						kiwi_enum2str(c->mode, mode_s, ARRAY_LEN(mode_s)), c->zoom, hr, min, sec, ext? ext:"", ip);
					if (user) free(user);
					if (geo) free(geo);
					if (ext) free(ext);
					n = 1;
				}
			}
			if (n == 0) {
				asprintf(&sb2, "%s{\"i\":%d}", need_comma? ",":"", i);
			}
			sb = kstr_cat(sb, kstr_wrap(sb2));
			need_comma = true;
		}

		sb = kstr_cat(sb, "]");
		send_msg(conn, false, "MSG user_cb=%s", kstr_sp(sb));
		kstr_free(sb);
		return true;
	}

#define DX_SPACING_ZOOM_THRESHOLD	5
#define DX_SPACING_THRESHOLD_PX		10
		dx_t *dp, *ldp, *upd;

	// SECURITY: should be okay: checks for conn->auth_admin first
	if (kiwi_str_begins_with(cmd, "SET DX_UPD")) {
		if (conn->auth_admin == false) {
			cprintf(conn, "DX_UPD NO ADMIN AUTH %s\n", conn->remote_ip);
			return true;
		}
		
		if (dx.len == 0) {
			return true;
		}
		
		float freq;
		int gid, mkr_off, flags, new_len;
		flags = 0;

		char *text_m, *notes_m;
		text_m = notes_m = NULL;
		n = sscanf(cmd, "SET DX_UPD g=%d f=%f o=%d m=%d i=%1024ms n=%1024ms", &gid, &freq, &mkr_off, &flags, &text_m, &notes_m);
        //printf("DX_UPD [%s]\n", cmd);
		//printf("DX_UPD n=%d #%d %8.2f 0x%x text=<%s> notes=<%s>\n", n, gid, freq, flags, text_m, notes_m);

		if (n != 2 && n != 6) {
			printf("DX_UPD n=%d\n", n);
            free(text_m); free(notes_m);
			return true;
		}
		
		//  gid freq    action
		//  !-1 -1      delete
		//  !-1 !-1     modify
		//  -1  x       add new
		
		dx_t *dxp;
		if (gid >= -1 && gid < dx.len) {
			if (gid != -1 && freq == -1) {
				// delete entry by forcing to top of list, then decreasing size by one before save
				cprintf(conn, "DX_UPD %s delete entry #%d\n", conn->remote_ip, gid);
				dxp = &dx.list[gid];
				dxp->freq = 999999;
				new_len = dx.len - 1;
			} else {
				if (gid == -1) {
					// new entry: add to end of list (in hidden slot), then sort will insert it properly
					cprintf(conn, "DX_UPD %s adding new entry\n", conn->remote_ip);
					assert(dx.hidden_used == false);		// FIXME need better serialization
					dxp = &dx.list[dx.len];
					dx.hidden_used = true;
					dx.len++;
					new_len = dx.len;
				} else {
					// modify entry
					cprintf(conn, "DX_UPD %s modify entry #%d\n", conn->remote_ip, gid);
					dxp = &dx.list[gid];
					new_len = dx.len;
				}
				dxp->freq = freq;
				dxp->offset = mkr_off;
				dxp->flags = flags;
				
				// remove trailing 'x' transmitted with text and notes fields
				text_m[strlen(text_m)-1] = '\0';
				notes_m[strlen(notes_m)-1] = '\0';
				
				// can't use kiwi_strdup because free() must be used later on
				dxp->ident = strdup(text_m);
				dxp->notes = strdup(notes_m);
			}
		} else {
			printf("DX_UPD: gid %d >= dx.len %d ?\n", gid, dx.len);
		}
		
		qsort(dx.list, dx.len, sizeof(dx_t), qsort_floatcomp);
		//printf("DX_UPD after qsort dx.len %d new_len %d top elem f=%.2f\n",
		//	dx.len, new_len, dx.list[dx.len-1].freq);
		dx.len = new_len;
		dx_save_as_json();		// FIXME need better serialization
		dx_reload();
		send_msg(conn, false, "MSG request_dx_update");	// get client to request updated dx list

        free(text_m); free(notes_m);
		return true;
	}

	if (kiwi_str_begins_with(cmd, "SET MKR")) {
		float min, max;
		int zoom, width;
		n = sscanf(cmd, "SET MKR min=%f max=%f zoom=%d width=%d", &min, &max, &zoom, &width);
		if (n != 4) return true;
		float bw;
		bw = max - min;
		static bool first = true;
		static int dx_lastx;
		dx_lastx = 0;
		time_t t; time(&t);
		
		if (dx.len == 0) {
			return true;
		}
		
		asprintf(&sb, "[{\"t\":%ld}", t);		// reset appending
		sb = kstr_wrap(sb);

		for (dp = dx.list, i=j=0; i < dx.len; dp++, i++) {
			float freq = dp->freq + (dp->offset / 1000.0);		// carrier plus offset

			// when zoomed far-in need to look at wider window since we don't know PB center here
			#define DX_SEARCH_WINDOW 10.0
			if (freq < min - DX_SEARCH_WINDOW) continue;
			if (freq > max + DX_SEARCH_WINDOW) break;
			
			// reduce dx label clutter
			if (zoom <= DX_SPACING_ZOOM_THRESHOLD) {
				int x = ((dp->freq - min) / bw) * width;
				int diff = x - dx_lastx;
				//printf("DX spacing %d %d %d %s\n", dx_lastx, x, diff, dp->ident);
				if (!first && diff < DX_SPACING_THRESHOLD_PX) continue;
				dx_lastx = x;
				first = false;
			}
			
			// NB: ident and notes are already stored URL encoded
			float f = dp->freq + (dp->offset / 1000.0);
			asprintf(&sb2, ",{\"g\":%d,\"f\":%.3f,\"o\":%.0f,\"b\":%d,\"i\":\"%s\"%s%s%s}",
				i, freq, dp->offset, dp->flags, dp->ident,
				dp->notes? ",\"n\":\"":"", dp->notes? dp->notes:"", dp->notes? "\"":"");
			//printf("dx(%d,%.3f,%.0f,%d,\'%s\'%s%s%s)\n", i, f, dp->offset, dp->flags, dp->ident,
			//	dp->notes? ",\'":"", dp->notes? dp->notes:"", dp->notes? "\'":"");
			sb = kstr_cat(sb, kstr_wrap(sb2));
		}
		
		sb = kstr_cat(sb, "]");
		send_msg(conn, false, "MSG mkr=%s", kstr_sp(sb));
		kstr_free(sb);
		return true;
	}

	if (strcmp(cmd, "SET GET_CONFIG") == 0) {
		asprintf(&sb, "{\"r\":%d,\"g\":%d,\"s\":%d,\"pu\":\"%s\",\"pe\":%d,\"pv\":\"%s\",\"pi\":%d,\"n\":%d,\"m\":\"%s\",\"v1\":%d,\"v2\":%d}",
			RX_CHANS, GPS_CHANS, ddns.serno, ddns.ip_pub, ddns.port_ext, ddns.ip_pvt, ddns.port, ddns.nm_bits, ddns.mac, version_maj, version_min);
		send_msg(conn, false, "MSG config_cb=%s", sb);
		free(sb);
		return true;
	}
	
	if (kiwi_str_begins_with(cmd, "SET STATS_UPD")) {
		int ch;
		n = sscanf(cmd, "SET STATS_UPD ch=%d", &ch);
		if (n != 1 || ch < 0 || ch > RX_CHANS) return true;

		rx_chan_t *rx;
		int underruns = 0, seq_errors = 0;
		n = 0;
		//n = snprintf(oc, rem, "{\"a\":["); oc += n; rem -= n;
		
		for (rx = rx_channels, i=0; rx < &rx_channels[RX_CHANS]; rx++, i++) {
			if (rx->busy) {
				conn_t *c = rx->conn_snd;
				if (c && c->valid && c->arrived && c->user != NULL) {
					underruns += c->audio_underrun;
					seq_errors += c->sequence_errors;
				}
			}
		}
		
		if (cpu_stats_buf != NULL) {
			asprintf(&sb, "{%s", cpu_stats_buf);
		} else {
			asprintf(&sb, "");
		}
		sb = kstr_wrap(sb);

		float sum_kbps = audio_kbps + waterfall_kbps + http_kbps;
		asprintf(&sb2, ",\"aa\":%.0f,\"aw\":%.0f,\"af\":%.0f,\"at\":%.0f,\"ah\":%.0f,\"as\":%.0f",
			audio_kbps, waterfall_kbps, waterfall_fps[ch], waterfall_fps[RX_CHANS], http_kbps, sum_kbps);
		sb = kstr_cat(sb, kstr_wrap(sb2));

		asprintf(&sb2, ",\"ga\":%d,\"gt\":%d,\"gg\":%d,\"gf\":%d,\"gc\":%.6f,\"go\":%d",
			gps.acquiring, gps.tracking, gps.good, gps.fixes, adc_clock_system()/1e6, clk.adc_clk_corrections);
		sb = kstr_cat(sb, kstr_wrap(sb2));

		extern int audio_dropped;
		asprintf(&sb2, ",\"ad\":%d,\"au\":%d,\"ae\":%d,\"ar\":%d,\"an\":%d,\"ap\":[",
			audio_dropped, underruns, seq_errors, dpump_resets, NRX_BUFS);
		sb = kstr_cat(sb, kstr_wrap(sb2));
		for (i = 0; i < NRX_BUFS; i++) {
		    asprintf(&sb2, "%s%d", (i != 0)? ",":"", dpump_hist[i]);
		    sb = kstr_cat(sb, kstr_wrap(sb2));
		}
        sb = kstr_cat(sb, "]");

		char *s, utc_s[32], local_s[32];
		time_t utc; time(&utc);
		s = asctime(gmtime(&utc));
		strncpy(utc_s, &s[11], 5);
		utc_s[5] = '\0';
		if (utc_offset != -1 && dst_offset != -1) {
			time_t local = utc + utc_offset + dst_offset;
			s = asctime(gmtime(&local));
			strncpy(local_s, &s[11], 5);
			local_s[5] = '\0';
		} else {
			strcpy(local_s, "");
		}
		asprintf(&sb2, ",\"tu\":\"%s\",\"tl\":\"%s\",\"ti\":\"%s\",\"tn\":\"%s\"",
			utc_s, local_s, tzone_id, tzone_name);
		sb = kstr_cat(sb, kstr_wrap(sb2));

		asprintf(&sb2, "}");
		sb = kstr_cat(sb, kstr_wrap(sb2));

		send_msg(conn, false, "MSG stats_cb=%s", kstr_sp(sb));
		kstr_free(sb);
		return true;
	}

	n = strcmp(cmd, "SET gps_update");
	if (n == 0) {
		gps_stats_t::gps_chan_t *c;
		
		asprintf(&sb, "{\"FFTch\":%d,\"ch\":[", gps.FFTch);
		sb = kstr_wrap(sb);
		
		for (i=0; i < gps_chans; i++) {
			c = &gps.ch[i];
			int un = c->ca_unlocked;
			asprintf(&sb2, "%s{ \"ch\":%d,\"prn\":%d,\"snr\":%d,\"rssi\":%d,\"gain\":%d,\"hold\":%d,\"wdog\":%d"
				",\"unlock\":%d,\"parity\":%d,\"sub\":%d,\"sub_renew\":%d,\"novfl\":%d}",
				i? ", ":"", i, c->prn, c->snr, c->rssi, c->gain, c->hold, c->wdog,
				un, c->parity, c->sub, c->sub_renew, c->novfl);
			sb = kstr_cat(sb, kstr_wrap(sb2));
			c->parity = 0;
			for (j = 0; j < SUBFRAMES; j++) {
				if (c->sub_renew & (1<<j)) {
					c->sub |= 1<<j;
					c->sub_renew &= ~(1<<j);
				}
			}
		}

		UMS hms(gps.StatSec/60/60);
		
		unsigned r = (timer_ms() - gps.start)/1000;
		if (r >= 3600) {
			asprintf(&sb2, "],\"run\":\"%d:%02d:%02d\"", r / 3600, (r / 60) % 60, r % 60);
		} else {
			asprintf(&sb2, "],\"run\":\"%d:%02d\"", (r / 60) % 60, r % 60);
		}
		sb = kstr_cat(sb, kstr_wrap(sb2));

		if (gps.ttff) {
			asprintf(&sb2, ",\"ttff\":\"%d:%02d\"", gps.ttff / 60, gps.ttff % 60);
		} else {
			asprintf(&sb2, ",\"ttff\":null");
		}
		sb = kstr_cat(sb, kstr_wrap(sb2));

		if (gps.StatDay != -1) {
			asprintf(&sb2, ",\"gpstime\":\"%s %02d:%02d:%02.0f\"", Week[gps.StatDay], hms.u, hms.m, hms.s);
		} else {
			asprintf(&sb2, ",\"gpstime\":null");
		}
		sb = kstr_cat(sb, kstr_wrap(sb2));

		if (gps.tLS_valid) {
			asprintf(&sb2, ",\"utc_offset\":\"%+d sec\"", gps.delta_tLS);
		} else {
			asprintf(&sb2, ",\"utc_offset\":null");
		}
		sb = kstr_cat(sb, kstr_wrap(sb2));

		if (gps.StatLat) {
			asprintf(&sb2, ",\"lat\":\"%8.6f %c\"", gps.StatLat, gps.StatNS);
			sb = kstr_cat(sb, kstr_wrap(sb2));
			asprintf(&sb2, ",\"lon\":\"%8.6f %c\"", gps.StatLon, gps.StatEW);
			sb = kstr_cat(sb, kstr_wrap(sb2));
			asprintf(&sb2, ",\"alt\":\"%1.0f m\"", gps.StatAlt);
			sb = kstr_cat(sb, kstr_wrap(sb2));
			asprintf(&sb2, ",\"map\":\"<a href='http://wikimapia.org/#lang=en&lat=%8.6f&lon=%8.6f&z=18&m=b' target='_blank'>wikimapia.org</a>\"",
				gps.sgnLat, gps.sgnLon);
		} else {
			asprintf(&sb2, ",\"lat\":null");
		}
		sb = kstr_cat(sb, kstr_wrap(sb2));
			
		asprintf(&sb2, ",\"acq\":%d,\"track\":%d,\"good\":%d,\"fixes\":%d,\"adc_clk\":%.6f,\"adc_corr\":%d}",
			gps.acquiring? 1:0, gps.tracking, gps.good, gps.fixes, adc_clock_system()/1e6, clk.adc_clk_corrections);
		sb = kstr_cat(sb, kstr_wrap(sb2));

		send_msg_encoded(conn, "MSG", "gps_update_cb", "%s", kstr_sp(sb));
		kstr_free(sb);
		return true;
	}

	// SECURITY: FIXME: get rid of this?
	int wf_comp;
	n = sscanf(cmd, "SET wf_comp=%d", &wf_comp);
	if (n == 1) {
		c2s_waterfall_compression(conn->rx_channel, wf_comp? true:false);
		printf("### SET wf_comp=%d\n", wf_comp);
		return true;
	}

	if (kiwi_str_begins_with(cmd, "SET pref_export")) {
		free(conn->pref_id);
		free(conn->pref);
		n = sscanf(cmd, "SET pref_export id=%64ms pref=%4096ms", &conn->pref_id, &conn->pref);
		if (n != 2) {
			cprintf(conn, "pref_export n=%d\n", n);
			return true;
		}
		cprintf(conn, "pref_export id=<%s> pref= %d <%s>\n",
			conn->pref_id, strlen(conn->pref), conn->pref);

		// remove prior exports from other channels
		conn_t *c;
		for (c = conns; c < &conns[N_CONNS]; c++) {
			if (c == conn) continue;
			if (c->pref_id && c->pref && strcmp(c->pref_id, conn->pref_id) == 0) {
			    free(c->pref_id); c->pref_id = NULL;
			    free(c->pref); c->pref = NULL;
			}
		}
		
		return true;
	}
	
	if (kiwi_str_begins_with(cmd, "SET pref_import")) {
		free(conn->pref_id);
		n = sscanf(cmd, "SET pref_import id=%64ms", &conn->pref_id);
		if (n != 1) {
			cprintf(conn, "pref_import n=%d\n", n);
			return true;
		}
		cprintf(conn, "pref_import id=<%s>\n", conn->pref_id);

		conn_t *c;
		for (c = conns; c < &conns[N_CONNS]; c++) {
			// allow self-match if (c == conn) continue;
			if (c->pref_id && c->pref && strcmp(c->pref_id, conn->pref_id) == 0) {
				cprintf(conn, "pref_import ch%d MATCH ch%d\n", conn->rx_channel, c->rx_channel);
				send_msg(conn, false, "MSG pref_import_ch=%d pref_import=%s", c->rx_channel, c->pref);
				break;
			}
		}
		if (c == &conns[N_CONNS]) {
			cprintf(conn, "pref_import NOT FOUND\n", conn->pref_id);
			send_msg(conn, false, "MSG pref_import=null");
		}

		free(conn->pref_id); conn->pref_id = NULL;
		return true;
	}

	if (kiwi_str_begins_with(cmd, "SET ident_user=")) {
        char *ident_user_m = NULL;
	    sscanf(cmd, "SET ident_user=%256ms", &ident_user_m);
		bool noname = (ident_user_m == NULL || ident_user_m[0] == '\0');
		bool setUserIP = false;
		
		if (conn->mc == NULL) return true;	// we've seen this
		if (noname && !conn->user) setUserIP = true;
		if (noname && conn->user && strcmp(conn->user, conn->remote_ip)) setUserIP = true;

		if (setUserIP) {
			kiwi_str_redup(&conn->user, "user", conn->remote_ip);
			conn->isUserIP = TRUE;
			// printf(">>> isUserIP TRUE: %s:%05d setUserIP=%d noname=%d user=%s <%s>\n",
			// 	conn->remote_ip, conn->remote_port, setUserIP, noname, conn->user, cmd);
		}

		if (!noname) {
			kiwi_str_decode_inplace(ident_user_m);
			kiwi_str_redup(&conn->user, "user", ident_user_m);
			conn->isUserIP = FALSE;
			// printf(">>> isUserIP FALSE: %s:%05d setUserIP=%d noname=%d user=%s <%s>\n",
			// 	conn->remote_ip, conn->remote_port, setUserIP, noname, conn->user, cmd);
		}
		
		//clprintf(conn, "SND user: <%s>\n", cmd);
		if (!conn->arrived) {
			loguser(conn, LOG_ARRIVED);
			conn->arrived = TRUE;
		}
		
		free(ident_user_m);
		return true;
	}

	n = sscanf(cmd, "SET need_status=%d", &j);
	if (n == 1) {
		if (conn->mc == NULL) return true;	// we've seen this
		char *status = (char*) cfg_string("status_msg", NULL, CFG_REQUIRED);
		send_msg_encoded(conn, "MSG", "status_msg_html", "\f%s", status);
		cfg_string_free(status);
		return true;
	}
	
	char *geo_m = NULL;
	n = sscanf(cmd, "SET geo=%127ms", &geo_m);
	if (n == 1) {
		kiwi_str_decode_inplace(geo_m);
		//cprintf(conn, "ch%d recv geoloc from client: %s\n", conn->rx_channel, geo_m);
		free(conn->geo);
		conn->geo = geo_m;
		return true;
	}

	char *geojson_m = NULL;
	n = sscanf(cmd, "SET geojson=%256ms", &geojson_m);
	if (n == 1) {
		kiwi_str_decode_inplace(geojson_m);
		//clprintf(conn, "SND geo: <%s>\n", geojson_m);
		free(geojson_m);
		return true;
	}
	
	char *browser_m = NULL;
	n = sscanf(cmd, "SET browser=%256ms", &browser_m);
	if (n == 1) {
		kiwi_str_decode_inplace(browser_m);
		//clprintf(conn, "SND browser: <%s>\n", browser_m);
		free(browser_m);
		return true;
	}
	
	int inactivity_timeout;
	n = sscanf(cmd, "SET OVERRIDE inactivity_timeout=%d", &inactivity_timeout);
	if (n == 1) {
		clprintf(conn, "SET OVERRIDE inactivity_timeout=%d\n", inactivity_timeout);
		if (inactivity_timeout == 0)
			conn->inactivity_timeout_override = true;
		return true;
	}

    int clk_adj;
    n = sscanf(cmd, "SET clk_adj=%d", &clk_adj);
    if (n == 1) {
		if (conn->auth_admin == false) {
			cprintf(conn, "clk_adj NO ADMIN AUTH %s\n", conn->remote_ip);
			return true;
		}
		
        int hz_limit = PPM_TO_HZ(ADC_CLOCK_NOM, ADC_CLOCK_PPM_LIMIT);
        if (clk_adj < -hz_limit || clk_adj > hz_limit) {
			cprintf(conn, "clk_adj TOO LARGE = %d %d %d %f\n", clk_adj, -hz_limit, hz_limit, PPM_TO_HZ(ADC_CLOCK_NOM, ADC_CLOCK_PPM_LIMIT));
			return true;
		}
		
        clock_manual_adj(clk_adj);
        printf("MANUAL clk_adj = %d\n", clk_adj);
		return true;
    }

	// SECURITY: only used during debugging
	n = sscanf(cmd, "SET nocache=%d", &i);
	if (n == 1) {
		web_nocache = i? true : false;
		printf("SET nocache=%d\n", web_nocache);
		return true;
	}

	// SECURITY: only used during debugging
	n = sscanf(cmd, "SET ctrace=%d", &i);
	if (n == 1) {
		web_caching_debug = i? true : false;
		printf("SET ctrace=%d\n", web_caching_debug);
		return true;
	}

	// SECURITY: only used during debugging
	n = sscanf(cmd, "SET debug_v=%d", &i);
	if (n == 1) {
		debug_v = i;
		//printf("SET debug_v=%d\n", debug_v);
		return true;
	}

	// SECURITY: only used during debugging
	sb = (char *) "SET debug_msg=";
	slen = strlen(sb);
	n = strncmp(cmd, sb, slen);
	if (n == 0) {
		kiwi_str_decode_inplace(cmd);
		clprintf(conn, "### DEBUG MSG: <%s>\n", &cmd[slen]);
		return true;
	}

	if (kiwi_str_begins_with(cmd, "SERVER DE CLIENT")) return true;
	
	// we see these sometimes; not part of our protocol
	if (strcmp(cmd, "PING") == 0) return true;

	// we see these at the close of a connection sometimes; not part of our protocol
    #define ASCII_ETX 3
	if (strlen(cmd) == 2 && cmd[0] == ASCII_ETX) return true;

	return false;
}
