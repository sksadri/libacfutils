/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
*/
/*
 * Copyright 2018 Saso Kiselkov. All rights reserved.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#if	IBM
#include <io.h>
#include <fcntl.h>
#endif

#include "acfutils/assert.h"
#include "acfutils/avl.h"
#include "acfutils/chartdb.h"
#include "acfutils/helpers.h"
#include "acfutils/list.h"
#include "acfutils/png.h"
#include "acfutils/thread.h"
#include "acfutils/worker.h"

#include "chartdb_impl.h"
#include "chart_prov_faa.h"

#define	MAX_METAR_AGE	60	/* seconds */
#define	MAX_TAF_AGE	300	/* seconds */
#define	RETRY_INTVAL	30	/* seconds */

static chart_prov_t prov[NUM_PROVIDERS] = {
    {
	.name = "aeronav.faa.gov",
	.init = chart_faa_init,
	.fini = chart_faa_fini,
	.get_chart = chart_faa_get_chart,
	.get_metar = chart_faa_get_metar,
	.get_taf = chart_faa_get_taf
    }
};

static chart_arpt_t *arpt_find(chartdb_t *cdb, const char *icao);

static int
chart_name_compar(const void *a, const void *b)
{
	const chart_t *ca = a, *cb = b;
	int res = strcmp(ca->name, cb->name);

	if (res < 0)
		return (-1);
	if (res == 0)
		return (0);
	return (1);
}

static int
arpt_compar(const void *a, const void *b)
{
	const chart_arpt_t *ca = a, *cb = b;
	int res = strcmp(ca->icao, cb->icao);

	if (res < 0)
		return (-1);
	if (res == 0)
		return (0);
	return (1);
}

static void
chart_destroy(chart_t *chart)
{
	if (chart->surf != NULL)
		cairo_surface_destroy(chart->surf);
	free(chart->name);
	free(chart->codename);
	free(chart->filename);
	free(chart);
}

static void
arpt_destroy(chart_arpt_t *arpt)
{
	void *cookie;
	chart_t *chart;

	cookie = NULL;
	while ((chart = avl_destroy_nodes(&arpt->charts, &cookie)) != NULL)
		chart_destroy(chart);
	avl_destroy(&arpt->charts);

	free(arpt->name);
	free(arpt->city);
	free(arpt->metar);
	free(arpt->taf);

	free(arpt);
}

static bool_t
loader_init(void *userinfo)
{
	chartdb_t *cdb = userinfo;

	ASSERT(cdb != NULL);
	ASSERT3U(cdb->prov, <, NUM_PROVIDERS);

	if (!prov[cdb->prov].init(cdb))
		return (B_FALSE);

	return (B_TRUE);
}

static void
loader_purge(chartdb_t *cdb)
{
	for (chart_arpt_t *arpt = avl_first(&cdb->arpts); arpt != NULL;
	    arpt = AVL_NEXT(&cdb->arpts, arpt)) {
		for (chart_t *chart = avl_first(&arpt->charts); chart != NULL;
		    chart = AVL_NEXT(&arpt->charts, chart)) {
			if (chart->surf != NULL) {
				cairo_surface_destroy(chart->surf);
				chart->surf = NULL;
			}
		}
	}
	while (list_remove_head(&cdb->load_seq) != NULL)
		;
}

chart_arpt_t *
chartdb_add_arpt(chartdb_t *cdb, const char *icao, const char *name,
    const char *city_name, const char *state_id)
{
	chart_arpt_t *arpt, srch;
	avl_index_t where;

	ASSERT(cdb != NULL);

	strlcpy(srch.icao, icao, sizeof (srch.icao));

	mutex_enter(&cdb->lock);
	arpt = avl_find(&cdb->arpts, &srch, &where);
	if (arpt == NULL) {
		arpt = calloc(1, sizeof (*arpt));
		avl_create(&arpt->charts, chart_name_compar, sizeof (chart_t),
		    offsetof(chart_t, node));
		strlcpy(arpt->icao, icao, sizeof (arpt->icao));
		arpt->name = strdup(name);
		arpt->city = strdup(city_name);
		strlcpy(arpt->state, state_id, sizeof (arpt->state));
		arpt->db = cdb;
		avl_insert(&cdb->arpts, arpt, where);
	}
	mutex_exit(&cdb->lock);

	return (arpt);
}

bool_t
chartdb_add_chart(chart_arpt_t *arpt, chart_t *chart)
{
	avl_index_t where;
	chartdb_t *cdb = arpt->db;

	ASSERT(cdb != NULL);

	mutex_enter(&cdb->lock);
	if (avl_find(&arpt->charts, chart, &where) != NULL) {
		mutex_exit(&cdb->lock);
		return (B_FALSE);
	}
	avl_insert(&arpt->charts, chart, where);
	chart->arpt = arpt;
	chart->num_pages = -1;
	mutex_exit(&cdb->lock);

	return (B_TRUE);
}

char *
chartdb_mkpath(chart_t *chart)
{
	chart_arpt_t *arpt = chart->arpt;
	chartdb_t *cdb;
	char airac_nr[8];

	ASSERT(arpt != NULL);
	cdb = arpt->db;
	ASSERT(cdb != NULL);

	snprintf(airac_nr, sizeof (airac_nr), "%d", cdb->airac);
	if (cdb->flat_db) {
		return (mkpathname(cdb->path, prov[cdb->prov].name, airac_nr,
		    chart->filename, NULL));
	} else {
		return (mkpathname(cdb->path, prov[cdb->prov].name, airac_nr,
		    arpt->icao, chart->filename, NULL));
	}
}

static int
pdf_count_pages(const char *pdfinfo_path, const char *path)
{
	int num_pages = -1;
	char cmd[3 * MAX_PATH];
	char *dpath = lacf_dirname(pdfinfo_path);
	FILE *fp;
	char *line = NULL;
	size_t cap = 0;

#if	IBM
	SECURITY_ATTRIBUTES sa;
	HANDLE stdout_rd_handle = NULL;
	HANDLE stdout_wr_handle = NULL;
	PROCESS_INFORMATION pi;
	STARTUPINFO si;
	TCHAR cmdT[3 * MAX_PATH];
	int fd;

	snprintf(cmd, sizeof (cmd), "\"%s\" \"%s\"", pdfinfo_path, path);
	MultiByteToWideChar(CP_UTF8, 0, cmd, -1, cmdT, 3 * MAX_PATH);

	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	if (!CreatePipe(&stdout_rd_handle, &stdout_wr_handle, &sa, 0) ||
	    !SetHandleInformation(stdout_rd_handle, HANDLE_FLAG_INHERIT, 0)) {
		win_perror(GetLastError(), "Error creating pipe");
		if (stdout_rd_handle != NULL)
			CloseHandle(stdout_rd_handle);
		if (stdout_wr_handle != NULL)
			CloseHandle(stdout_wr_handle);
		goto errout;
	}
	ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
	ZeroMemory(&si, sizeof(STARTUPINFO));
	si.cb = sizeof(STARTUPINFO);
	si.hStdOutput = stdout_wr_handle;
	si.dwFlags |= STARTF_USESTDHANDLES;

	fd = _open_osfhandle((intptr_t)stdout_rd_handle, _O_RDONLY);
	if (fd == -1) {
		win_perror(GetLastError(), "Error opening pipe as fd");
		CloseHandle(stdout_rd_handle);
		CloseHandle(stdout_wr_handle);
		goto errout;
	}
	fp = _fdopen(fd, "r");
	if (fp == NULL) {
		win_perror(GetLastError(), "Error opening fd as fp");
		CloseHandle(stdout_rd_handle);
		CloseHandle(stdout_wr_handle);
		goto errout;
	}
	if (!CreateProcess(NULL, cmdT, NULL, NULL, TRUE,
	    CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
	    NULL, NULL, &si, &pi)) {
		win_perror(GetLastError(), "Error invoking %s", pdfinfo_path);
		CloseHandle(stdout_rd_handle);
		CloseHandle(stdout_wr_handle);
		goto errout;
	}
#else	/* !IBM */

	snprintf(cmd, sizeof (cmd), "%sLD_LIBRARY_PATH=\"%s\" \"%s\" \"%s\"",
#if	APL
	    "DY",
#else	/* LIN */
	    "",
#endif	/* LIN */
	    dpath, pdfinfo_path, path);

	fp = popen(cmd, "r");
	if (fp == NULL)
		goto errout;
#endif	/* !IBM */

	while (getline(&line, &cap, fp) > 0) {
		if (strncmp(line, "Pages:", 6) == 0) {
			size_t n_comps;
			char **comps;

			strip_space(line);
			comps = strsplit(line, " ", B_TRUE, &n_comps);
			if (n_comps >= 2)
				num_pages = atoi(comps[1]);
			free_strlist(comps, n_comps);
			break;
		}
	}

	fclose(fp);

#if	IBM
	_close(fd);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(stdout_rd_handle);
	CloseHandle(stdout_wr_handle);
#endif	/* IBM */

errout:
	free(line);
	free(dpath);
	if (num_pages == -1)
		logMsg("Unable to read page count from %s", path);

	return (num_pages);
}

static char *
pdf_convert(const char *pdftoppm_path, char *old_path, int page, double zoom)
{
	char *ext, *new_path;
	char cmd[3 * MAX_PATH];
	char *dpath;
#if	IBM
	TCHAR cmdT[3 * MAX_PATH];
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD exit_code;
#endif	/* IBM */

	zoom = clamp(zoom, 0.1, 10.0);
	new_path = strdup(old_path);
	ext = strrchr(new_path, '.');
	VERIFY(ext != NULL);
	strlcpy(&ext[1], "png", strlen(&ext[1]) + 1);
	dpath = lacf_dirname(pdftoppm_path);

	/*
	 * As the PDF conversion process can be rather CPU-intensive, run
	 * with reduced priority to avoid starving the sim, even if that
	 * means taking longer.
	 */
#if	IBM
	snprintf(cmd, sizeof (cmd), "\"%s\" -png -f %d -l %d -r %d "
	    "\"%s\" > \"%s\"", pdftoppm_path, page + 1, page + 1,
	    (int)(100 * zoom), old_path, new_path);
	MultiByteToWideChar(CP_UTF8, 0, cmd, -1, cmdT, 3 * MAX_PATH);

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	if (!CreateProcess(NULL, cmdT, NULL, NULL, FALSE,
	    CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
	    NULL, NULL, &si, &pi)) {
		win_perror(GetLastError(), "Error converting chart %s to "
		    "PNG", old_path);
		goto errout;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	VERIFY(GetExitCodeProcess(pi.hProcess, &exit_code));
	if (exit_code != 0) {
		logMsg("Error converting chart %s to PNG. Command that "
		    "failed: \"%s\"", old_path, cmd);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		goto errout;
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
#else	/* !IBM */
	snprintf(cmd, sizeof (cmd), "%sLD_LIBRARY_PATH=\"%s\" nice \"%s\" "
	    "-png -f %d -l %d -r %d \"%s\" > \"%s\"",
#if	APL	/* Apply the Apple-specific prefix */
	    "DY",
#else
	    "",
#endif
	    dpath, pdftoppm_path, page + 1, page + 1, (int)(100 * zoom),
	    old_path, new_path);
	if (system(cmd) != 0) {
		logMsg("Error converting chart %s to PNG: %s. Command that "
		    "failed: \"%s\"", old_path, strerror(errno), cmd);
		goto errout;
	}
#endif	/* !IBM */
	free(old_path);
	free(dpath);
	return (new_path);
errout:
	free(dpath);
	free(new_path);
	free(old_path);
	return (NULL);
}

static void
invert_surface(cairo_surface_t *surf)
{
	uint8_t *data = cairo_image_surface_get_data(surf);
	int stride = cairo_image_surface_get_stride(surf);
	int width = cairo_image_surface_get_width(surf);
	int height = cairo_image_surface_get_height(surf);

	cairo_surface_flush(surf);

	switch (cairo_image_surface_get_format(surf)) {
	case CAIRO_FORMAT_ARGB32:
	case CAIRO_FORMAT_RGB24:
		for (int y = 0; y < height; y++) {
			uint8_t *p = data + y * stride;

			for (int x = 0; x < width; x++) {
#if	__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
				p[1] = 255 - p[1];
				p[2] = 255 - p[2];
				p[3] = 255 - p[3];
#else
				p[0] = 255 - p[0];
				p[1] = 255 - p[1];
				p[2] = 255 - p[2];
#endif
				p += 4;
			}
		}
		break;
	default:
		logMsg("Unable to invert surface colors: unsupported "
		    "format %x", cairo_image_surface_get_format(surf));
		break;
	}
	cairo_surface_mark_dirty(surf);
}

static void
loader_load(chartdb_t *cdb, chart_t *chart)
{
	char *path = chartdb_mkpath(chart);
	char *ext;
	cairo_surface_t *surf;
	cairo_status_t st;

	if (!chart->refreshed || !file_exists(path, NULL)) {
		chart->refreshed = B_TRUE;
		if (!prov[cdb->prov].get_chart(chart)) {
			mutex_enter(&cdb->lock);
			chart->load_error = B_TRUE;
			mutex_exit(&cdb->lock);
			free(path);
			return;
		}
	}

	ext = strrchr(path, '.');
	if (ext != NULL &&
	    (strcmp(&ext[1], "pdf") == 0 || strcmp(&ext[1], "PDF") == 0)) {
		if (chart->num_pages == -1) {
			chart->num_pages = pdf_count_pages(cdb->pdfinfo_path,
			    path);
		}
		if (chart->num_pages == -1) {
			mutex_enter(&cdb->lock);
			chart->load_error = B_TRUE;
			mutex_exit(&cdb->lock);
			goto out;
		}
		path = pdf_convert(cdb->pdftoppm_path, path, chart->load_page,
		    chart->zoom);
		if (path == NULL) {
			mutex_enter(&cdb->lock);
			chart->load_page = chart->cur_page;
			chart->load_error = B_TRUE;
			mutex_exit(&cdb->lock);
			goto out;
		}
	}

	surf = cairo_image_surface_create_from_png(path);
	if ((st = cairo_surface_status(surf)) == CAIRO_STATUS_SUCCESS) {
		if (chart->night)
			invert_surface(surf);
		mutex_enter(&cdb->lock);
		if (chart->surf != NULL)
			cairo_surface_destroy(chart->surf);
		chart->surf = surf;
		chart->cur_page = chart->load_page;
		mutex_exit(&cdb->lock);
	} else {
		logMsg("Can't load PNG file %s: %s", path,
		    cairo_status_to_string(st));
		mutex_enter(&cdb->lock);
		chart->load_error = B_TRUE;
		mutex_exit(&cdb->lock);
	}

out:
	free(path);
}

static bool_t
loader(void *userinfo)
{
	chartdb_t *cdb = userinfo;
	chart_t *chart;

	mutex_enter(&cdb->lock);
	while ((chart = list_remove_head(&cdb->loader_queue)) != NULL) {
		if (chart == &cdb->loader_cmd_purge) {
			loader_purge(cdb);
		} else if (chart == &cdb->loader_cmd_metar) {
			char *metar;

			chart->arpt->metar_load_t = time(NULL);
			mutex_exit(&cdb->lock);
			metar = prov[cdb->prov].get_metar(cdb,
			    chart->arpt->icao);
			mutex_enter(&cdb->lock);
			if (chart->arpt->metar != NULL)
				free(chart->arpt->metar);
			chart->arpt->metar = metar;
			if (metar == NULL) {
				chart->arpt->metar_load_t = time(NULL) -
				    (MAX_METAR_AGE - RETRY_INTVAL);
			}
		} else if (chart == &cdb->loader_cmd_taf) {
			char *taf;

			chart->arpt->taf_load_t = time(NULL);
			mutex_exit(&cdb->lock);
			taf = prov[cdb->prov].get_taf(cdb, chart->arpt->icao);
			mutex_enter(&cdb->lock);
			if (chart->arpt->taf != NULL)
				free(chart->arpt->taf);
			chart->arpt->taf = taf;
			if (taf == NULL) {
				chart->arpt->taf_load_t = time(NULL) -
				    (MAX_TAF_AGE - RETRY_INTVAL);
			}
		} else {
			mutex_exit(&cdb->lock);
			loader_load(cdb, chart);
			mutex_enter(&cdb->lock);
			/* Move to the head of the load sequence list */
			if (list_link_active(&chart->load_seq_node))
				list_remove(&cdb->load_seq, chart);
			list_insert_head(&cdb->load_seq, chart);
			while (list_count(&cdb->load_seq) > cdb->load_limit) {
				chart_t *c = list_tail(&cdb->load_seq);

				if (c->surf != NULL) {
					cairo_surface_destroy(c->surf);
					c->surf = NULL;
				}
				list_remove(&cdb->load_seq, c);
			}
		}
	}
	mutex_exit(&cdb->lock);

	return (B_TRUE);
}

static void
loader_fini(void *userinfo)
{
	chartdb_t *cdb = userinfo;

	ASSERT(cdb != NULL);
	ASSERT3U(cdb->prov, <, NUM_PROVIDERS);
	prov[cdb->prov].fini(cdb);
}

API_EXPORT chartdb_t *
chartdb_init(const char *cache_path, const char *pdftoppm_path,
    const char *pdfinfo_path, unsigned airac, const char *provider_name,
    void *provider_info)
{
	chartdb_t *cdb;
	chart_prov_id_t pid;

	for (pid = 0; pid < NUM_PROVIDERS; pid++) {
		if (strcmp(provider_name, prov[pid].name) == 0)
			break;
	}
	if (pid >= NUM_PROVIDERS)
		return (NULL);

	cdb = calloc(1, sizeof (*cdb));
	mutex_init(&cdb->lock);
	avl_create(&cdb->arpts, arpt_compar, sizeof (chart_arpt_t),
	    offsetof(chart_arpt_t, node));
	cdb->path = strdup(cache_path);
	cdb->pdftoppm_path = strdup(pdftoppm_path);
	cdb->pdfinfo_path = strdup(pdfinfo_path);
	cdb->airac = airac;
	cdb->prov_info = provider_info;
	cdb->load_limit = 4;
	strlcpy(cdb->prov_name, provider_name, sizeof (cdb->prov_name));

	list_create(&cdb->loader_queue, sizeof (chart_t),
	    offsetof(chart_t, loader_node));
	list_create(&cdb->load_seq, sizeof (chart_t),
	    offsetof(chart_t, load_seq_node));

	worker_init2(&cdb->loader, loader_init, loader, loader_fini, 0, cdb,
	    "chartdb");

	return (cdb);
}

API_EXPORT
void chartdb_fini(chartdb_t *cdb)
{
	void *cookie;
	chart_arpt_t *arpt;

	worker_fini(&cdb->loader);

	while(list_remove_head(&cdb->load_seq) != NULL)
		;
	while(list_remove_head(&cdb->loader_queue) != NULL)
		;

	cookie = NULL;
	while ((arpt = avl_destroy_nodes(&cdb->arpts, &cookie)) != NULL)
		arpt_destroy(arpt);
	avl_destroy(&cdb->arpts);
	mutex_destroy(&cdb->lock);

	free(cdb->path);
	free(cdb->pdftoppm_path);
	free(cdb->pdfinfo_path);
	free(cdb);
}

API_EXPORT void
chartdb_purge(chartdb_t *cdb)
{
	mutex_enter(&cdb->lock);

	/* purge the queue */
	while (list_remove_head(&cdb->loader_queue) != NULL)
		;
	list_insert_tail(&cdb->loader_queue, &cdb->loader_cmd_purge);
	worker_wake_up(&cdb->loader);

	mutex_exit(&cdb->lock);
}

API_EXPORT char **
chartdb_get_chart_names(chartdb_t *cdb, const char *icao, chart_type_t type,
    size_t *num_charts)
{
	chart_arpt_t *arpt;
	char **charts;
	chart_t *chart;
	size_t i, num;

	mutex_enter(&cdb->lock);

	arpt = arpt_find(cdb, icao);
	if (arpt == NULL) {
		mutex_exit(&cdb->lock);
		*num_charts = 0;
		return (NULL);
	}

	for (chart = avl_first(&arpt->charts), num = 0; chart != NULL;
	    chart = AVL_NEXT(&arpt->charts, chart)) {
		if (chart->type & type)
			num++;
	}
	if (num == 0) {
		mutex_exit(&cdb->lock);
		*num_charts = 0;
		return (NULL);
	}
	charts = calloc(num, sizeof (*charts));
	for (chart = avl_first(&arpt->charts), i = 0; chart != NULL;
	    chart = AVL_NEXT(&arpt->charts, chart)) {
		if (chart->type & type) {
			ASSERT3U(i, <, num);
			charts[i] = strdup(chart->name);
			i++;
		}
	}

	mutex_exit(&cdb->lock);

	*num_charts = num;

	return (charts);
}

API_EXPORT void
chartdb_free_str_list(char **l, size_t num)
{
	free_strlist(l, num);
}

static chart_arpt_t *
arpt_find(chartdb_t *cdb, const char *icao)
{
	chart_arpt_t srch;
	switch (strlen(icao)) {
	case 3:
		/*
		 * In the US it's common to omit the leading 'K', especially
		 * for non-ICAO airports. Adapt to them.
		 */
		snprintf(srch.icao, sizeof (srch.icao), "K%s", icao);
		break;
	case 4:
		strlcpy(srch.icao, icao, sizeof (srch.icao));
		break;
	default:
		return (NULL);
	}
	return (avl_find(&cdb->arpts, &srch, NULL));
}

static chart_t *
chart_find(chartdb_t *cdb, const char *icao, const char *chart_name)
{
	chart_arpt_t *arpt = arpt_find(cdb, icao);
	chart_t srch_chart = { .name = (char *)chart_name };
	if (arpt == NULL)
		return (NULL);
	return (avl_find(&arpt->charts, &srch_chart, NULL));
}

API_EXPORT bool_t
chartdb_get_chart_surface(chartdb_t *cdb, const char *icao,
    const char *chart_name, int page, double zoom, bool_t night,
    cairo_surface_t **surf, int *num_pages)
{
	chart_t *chart;

	mutex_enter(&cdb->lock);

	chart = chart_find(cdb, icao, chart_name);
	if (chart == NULL || chart->load_error) {
		mutex_exit(&cdb->lock);
		return (B_FALSE);
	}

	if ((chart->surf == NULL || chart->zoom != zoom ||
	    chart->night != night || chart->cur_page != page) &&
	    !list_link_active(&chart->loader_node)) {
		chart->zoom = zoom;
		chart->load_page = page;
		chart->night = night;
		list_insert_tail(&cdb->loader_queue, chart);
		worker_wake_up(&cdb->loader);
	}

	if (chart->surf != NULL && page == chart->cur_page)
		*surf = cairo_surface_reference(chart->surf);
	else
		*surf = NULL;

	if (num_pages != NULL)
		*num_pages = chart->num_pages;

	mutex_exit(&cdb->lock);

	return (B_TRUE);
}

static char *
get_metar_taf_common(chartdb_t *cdb, const char *icao, bool_t metar)
{
	char *result = NULL;
	time_t now = time(NULL);
	chart_arpt_t *arpt;

	mutex_enter(&cdb->lock);

	arpt = arpt_find(cdb, icao);
	if (arpt == NULL) {
		mutex_exit(&cdb->lock);
		return (NULL);
	}

	/*
	 * We could have NULLs in the cache here if the download
	 * failed. In that case, wait a little before retrying
	 * another download.
	 */
	if (metar && now - arpt->metar_load_t < MAX_METAR_AGE) {
		/* Fresh METAR still cached, return that. */
		if (arpt->metar != NULL)
			result = strdup(arpt->metar);
	} else if (!metar && now - arpt->taf_load_t < MAX_TAF_AGE) {
		/* Fresh TAF still cached, return that. */
		if (arpt->taf != NULL)
			result = strdup(arpt->taf);
	} else {
		if (metar) {
			if (!list_link_active(
			    &cdb->loader_cmd_metar.loader_node)) {
				/* Initiate async download of METAR */
				cdb->loader_cmd_metar.arpt = arpt;
				list_insert_tail(&cdb->loader_queue,
				    &cdb->loader_cmd_metar);
				worker_wake_up(&cdb->loader);
			}
			/* If we have an old METAR, return that for now */
			if (arpt->metar != NULL)
				result = strdup(arpt->metar);
		} else {
			if (!list_link_active(
			    &cdb->loader_cmd_taf.loader_node)) {
				/* Initiate async download of TAF */
				cdb->loader_cmd_taf.arpt = arpt;
				list_insert_tail(&cdb->loader_queue,
				    &cdb->loader_cmd_taf);
				worker_wake_up(&cdb->loader);
			}
			/* If we have an old TAF, return that for now */
			if (arpt->taf != NULL)
				result = strdup(arpt->taf);
		}
	}

	mutex_exit(&cdb->lock);
	return (result);
}

#define	ARPT_GET_COMMON(field_name) \
	do { \
		chart_arpt_t *arpt; \
		char *field_name; \
		mutex_enter(&cdb->lock); \
		arpt = arpt_find(cdb, icao); \
		if (arpt == NULL) { \
			mutex_exit(&cdb->lock); \
			return (NULL); \
		} \
		field_name = strdup(arpt->field_name); \
		mutex_exit(&cdb->lock); \
		return (field_name); \
	} while (0)

API_EXPORT char *
chartdb_get_arpt_name(chartdb_t *cdb, const char *icao)
{
	ARPT_GET_COMMON(name);
}

API_EXPORT char *
chartdb_get_arpt_city(chartdb_t *cdb, const char *icao)
{
	ARPT_GET_COMMON(city);
}

API_EXPORT char *
chartdb_get_arpt_state(chartdb_t *cdb, const char *icao)
{
	ARPT_GET_COMMON(state);
}

API_EXPORT char *
chartdb_get_metar(chartdb_t *cdb, const char *icao)
{
	return (get_metar_taf_common(cdb, icao, B_TRUE));
}

API_EXPORT char *
chartdb_get_taf(chartdb_t *cdb, const char *icao)
{
	return (get_metar_taf_common(cdb, icao, B_FALSE));
}