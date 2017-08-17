/*
    DeaDBeeF -- the music player
    Copyright (C) 2009-2017 Alexey Yakovenko and other contributors

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.

    3. This notice may not be removed or altered from any source distribution.
*/

#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <sys/time.h>
#include <math.h>
#include "interface.h"
#include "support.h"
#include "../../deadbeef.h"

#include "../rg_scanner/rg_scanner.h"

extern DB_functions_t *deadbeef;

typedef struct rgs_controller_s {
    GtkWidget *progress_window;
    GtkWidget *results_window;
    ddb_rg_scanner_settings_t _rg_settings;
    ddb_rg_scanner_t *_rg;
    int _abort_flag;
    struct timeval _rg_start_tv;
    int _abortTagWriting;
    struct rgs_controller_s *next;
} rgs_controller_t;

static rgs_controller_t *g_rgControllers;
static char *_title_tf;
static ddb_rg_scanner_t *_rg;

typedef struct {
    rgs_controller_t *ctl;
    int current;
} progress_data_t;

static float
_getScanSpeed (uint64_t cd_samples_processed, float time) {
    return cd_samples_processed / 44100.f / time;
}

static void
_formatTime (float sec, int extraPrecise, char *buf, int bufsize) {
    int hr;
    int min;
    hr = (int)floor (sec / 360);
    sec -= hr * 360;
    min = (int)floor (sec / 60);
    sec -= min * 60;

    if (extraPrecise) {
        if (hr > 0) {
            snprintf (buf, bufsize, "%d:%02d:%0.3f", hr, min, sec);
            return;
        }
        snprintf (buf, bufsize, "%02d:%0.3f", min, sec);
        return;
    }
    if (hr > 0) {
        snprintf (buf, bufsize, "%d:%02d:%02d", hr, min, (int)floor(sec));
        return;
    }
    snprintf (buf, bufsize, "%02d:%02d", min, (int)floor(sec));
}


static void
_ctl_progress (rgs_controller_t *ctl, int current) {
    deadbeef->pl_lock ();
    const char *uri = deadbeef->pl_find_meta (ctl->_rg_settings.tracks[current], ":URI");

    GtkWidget *progressText = lookup_widget (ctl->progress_window, "rg_scan_progress_file");
    gtk_entry_set_text (GTK_ENTRY (progressText), uri);
    GtkWidget *progressBar = lookup_widget (ctl->progress_window, "rg_scan_progress_bar");
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progressBar), (double)current/ctl->_rg_settings.num_tracks);
    GtkWidget *statusLabel = lookup_widget (ctl->progress_window, "rg_scan_progress_status");

    struct timeval tv;
    gettimeofday (&tv, NULL);
    float timePassed = (tv.tv_sec-ctl->_rg_start_tv.tv_sec) + (tv.tv_usec - ctl->_rg_start_tv.tv_usec) / 1000000.f;
    if (timePassed > 0 && ctl->_rg_settings.cd_samples_processed > 0 && current > 0) {
        float speed = _getScanSpeed (ctl->_rg_settings.cd_samples_processed, timePassed);
        float predicted_samples_total = ctl->_rg_settings.cd_samples_processed / (float)current * ctl->_rg_settings.num_tracks;

        float frac = (float)((double)predicted_samples_total / ctl->_rg_settings.cd_samples_processed);
        float est = timePassed * frac;

        char elapsed[50];
        _formatTime (timePassed, 0, elapsed, sizeof (elapsed));
        char estimated[50];
        _formatTime (est, 0, estimated, sizeof (estimated));

        char status[200];
        snprintf (status, sizeof (status), "Time elapsed: %s, estimated: %s, speed: %0.2fx", elapsed, estimated, speed);
        gtk_label_set_text (GTK_LABEL (statusLabel), status);
    }
    else {
        gtk_label_set_text (GTK_LABEL (statusLabel), "");
    }

    deadbeef->pl_unlock ();
}

static gboolean
_scan_progress_cb (void *ctx) {
    progress_data_t *dt = ctx;
    _ctl_progress (dt->ctl, dt->current);
    free (dt);
    return FALSE;
}

static void
_scan_progress (int current, void *user_data) {
    progress_data_t *dt = calloc (1, sizeof (progress_data_t));
    dt->current = current;
    dt->ctl = user_data;
    g_idle_add (_scan_progress_cb, dt);
}

static void
_ctl_dismiss (rgs_controller_t *ctl) {
    if (ctl->_rg_settings.tracks) {
        for (int i = 0; i < ctl->_rg_settings.num_tracks; i++) {
            deadbeef->pl_item_unref (ctl->_rg_settings.tracks[i]);
        }
        free (ctl->_rg_settings.tracks);
    }
    if (ctl->_rg_settings.results) {
        free (ctl->_rg_settings.results);
    }
    memset (&ctl->_rg_settings, 0, sizeof (ctl->_rg_settings));

    // remove from list    
    rgs_controller_t *prev = NULL;
    for (rgs_controller_t *c = g_rgControllers; c; c = c->next) {
        if (c == ctl) {
            if (prev) {
                prev->next = ctl->next;
            }
            else {
                g_rgControllers = ctl->next;
            }
            break;
        }
        prev = c;
    }

    free (ctl);
}

void _ctl_scanFinished (rgs_controller_t *ctl) {
    struct timeval tv;
    gettimeofday (&tv, NULL);
    float timePassed = (tv.tv_sec-ctl->_rg_start_tv.tv_sec) + (tv.tv_usec - ctl->_rg_start_tv.tv_usec) / 1000000.f;

    // TODO: display results table
}


static gboolean
_rgs_finished_cb (void *ctx) {
    rgs_controller_t *ctl = ctx;
    if (ctl->_abort_flag) {
        _ctl_dismiss (ctl);
        return FALSE;
    }
    _ctl_scanFinished (ctl);
    return FALSE;
}

static void
_rgs_job (void *ctx) {
    rgs_controller_t *ctl = ctx;
    _rg->scan (&ctl->_rg_settings);
    deadbeef->background_job_decrement ();

    g_idle_add (_rgs_finished_cb, ctl);
}

static void
runScanner (int mode, DB_playItem_t ** tracks, int count) {
    deadbeef->background_job_increment ();

    rgs_controller_t *ctl = calloc (1, sizeof (rgs_controller_t));

    if (!_title_tf) {
        _title_tf = deadbeef->tf_compile ("%title%");
    }

    _rg = (ddb_rg_scanner_t *)deadbeef->plug_get_for_id ("rg_scanner");
    if (_rg && _rg->misc.plugin.version_major != 1) {
        _rg = NULL;
        deadbeef->log ("Invalid version of rg_scanner plugin");
        return;
    }

    if (!_rg) {
        deadbeef->log ("ReplayGain plugin is not found");
        return;
    }

    ctl->progress_window = create_rg_scan_progress ();
    gtk_widget_show (ctl->progress_window);

    memset (&ctl->_rg_settings, 0, sizeof (ddb_rg_scanner_settings_t));
    ctl->_rg_settings._size = sizeof (ddb_rg_scanner_settings_t);
    ctl->_rg_settings.mode = mode;
    ctl->_rg_settings.tracks = tracks;
    ctl->_rg_settings.num_tracks = count;
    ctl->_rg_settings.ref_loudness = deadbeef->conf_get_float ("rg_scanner.target_db", DDB_RG_SCAN_DEFAULT_LOUDNESS);
    ctl->_rg_settings.results = calloc (count, sizeof (ddb_rg_scanner_result_t));
    ctl->_rg_settings.pabort = &ctl->_abort_flag;
    ctl->_rg_settings.progress_callback = _scan_progress;
    ctl->_rg_settings.progress_cb_user_data = ctl;

    gettimeofday (&ctl->_rg_start_tv, NULL);
    _ctl_progress (ctl, 0);


    // FIXME: need to use some sort of job queue
    uint64_t tid = deadbeef->thread_start (_rgs_job, NULL);
    deadbeef->thread_detach (tid);

    ctl->next = g_rgControllers;
    g_rgControllers = ctl;
}


int
action_rg_scan_per_file_handler (struct DB_plugin_action_s *action, int ctx) {
    return 0;
}

int
action_rg_remove_info_handler (struct DB_plugin_action_s *action, int ctx) {
    return 0;
}

int
action_rg_scan_selection_as_albums_handler (struct DB_plugin_action_s *action, int ctx) {
    return 0;
}

int
action_rg_scan_selection_as_album_handler (struct DB_plugin_action_s *action, int ctx) {
    return 0;
}


