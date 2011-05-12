/* 
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Authors:        Andrew Hutchings, SkySQL (andrew at skysql dot com)
*/

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <mysql.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <zlib.h>
#include "common.h"
#include "myloader.h"
#include "config.h"

guint commit_count= 1000;
gchar *directory= NULL;
gboolean overwrite_tables= FALSE;

static GMutex *init_mutex= NULL;

guint errors= 0;

gboolean read_data(FILE *file, gboolean is_compressed, GString *data, gboolean *eof);
void restore_data(MYSQL *conn, char *database, char *table, const char *filename);
void *process_queue(struct configuration *conf);
void add_table(const gchar* filename, struct configuration *conf);
void add_schema(const gchar* filename, MYSQL *conn);
void restore_databases(struct configuration *conf, MYSQL *conn);

static GOptionEntry entries[] =
{
	{ "directory", 'd', 0, G_OPTION_ARG_STRING, &directory, "Directory of the dump to import", NULL },
	{ "queries-per-transaction", 'q', 0, G_OPTION_ARG_INT, &commit_count, "Number of queries per transaction (default 1000)", NULL },
	{ "overwrite-tables", 'o', 0, G_OPTION_ARG_NONE, &overwrite_tables, "Drop tables if they already exist", NULL },
	{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int main(int argc, char *argv[]) {
	struct configuration conf= { NULL, NULL, NULL, 0 };

	GError *error= NULL;
	GOptionContext *context;

	g_thread_init(NULL);

	init_mutex= g_mutex_new();

	context= g_option_context_new("multi-threaded MySQL loader");
	GOptionGroup *main_group= g_option_group_new("main", "Main Options", "Main Options", NULL, NULL);
	g_option_group_add_entries(main_group, entries);
	g_option_group_add_entries(main_group, common_entries);
	g_option_context_set_main_group(context, main_group);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_print("option parsing failed: %s, try --help\n", error->message);
		exit(EXIT_FAILURE);
	}
	g_option_context_free(context);

	if (program_version) {
		g_print("myloader %s\n", VERSION);
		exit(EXIT_SUCCESS);
	}

	if (!directory) {
		g_critical("a directory needs to be specified, see --help\n");
		exit(EXIT_FAILURE);
	} else {
		char *p= g_strdup_printf("%s/.metadata", directory);
		if (!g_file_test(p, G_FILE_TEST_EXISTS)) {
			g_critical("the specified directory is not a mydumper backup\n");
			exit(EXIT_FAILURE);
		}
	}

	MYSQL *conn;
	conn= mysql_init(NULL);
	mysql_options(conn, MYSQL_READ_DEFAULT_GROUP, "myloader");

	if (!mysql_real_connect(conn, hostname, username, password, NULL, port, socket_path, 0)) {
		g_critical("Error connection to database: %s", mysql_error(conn));
		exit(EXIT_FAILURE);
	}
	mysql_query(conn, "/*!40014 SET FOREIGN_KEY_CHECKS=0*/");
	conf.queue= g_async_queue_new();
	conf.ready= g_async_queue_new();

	guint n;
	GThread **threads= g_new(GThread*, num_threads);
	for (n= 0; n < num_threads; n++) {
		threads[n]= g_thread_create((GThreadFunc)process_queue, &conf, TRUE, NULL);
		g_async_queue_pop(conf.ready);
	}
	g_async_queue_unref(conf.ready);

        restore_databases(&conf, conn);

	for (n= 0; n < num_threads; n++) {
		struct job *j= g_new0(struct job, 1);
		j->type = JOB_SHUTDOWN;
		g_async_queue_push(conf.queue, j);
	}

	for (n= 0; n < num_threads; n++) {
		g_thread_join(threads[n]);
	}

	g_async_queue_unref(conf.queue);
	mysql_close(conn);
	mysql_thread_end();
	mysql_library_end();
	g_free(directory);
	g_free(threads);

	return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}

void restore_databases(struct configuration *conf, MYSQL *conn) {
	GError *error= NULL;
	GDir* dir= g_dir_open(directory, 0, &error);

	if (error) {
		g_critical("cannot open directory %s, %s\n", directory, error->message);
		errors++;
		return;
	}

	const gchar* filename= NULL;

	while((filename= g_dir_read_name(dir))) {
		if (g_strrstr(filename, "-schema.sql")) {
			add_schema(filename, conn);
		}
	}

	g_dir_rewind(dir);

	while((filename= g_dir_read_name(dir))) {
		if (!g_strrstr(filename, "-schema.sql") && g_strrstr(filename, ".sql")) {
			add_table(filename, conf);
		}
	}

	g_dir_close(dir);
}

void add_schema(const gchar* filename, MYSQL *conn) {
	// 0 is database, 1 is table with -schema on the end
	gchar** split_file= g_strsplit(filename, ".", 0);
	gchar* database= split_file[0];
	// Remove the -schema from the table name
	gchar** split_table= g_strsplit(split_file[1], "-", 0);
	gchar* table= split_table[0];

	gchar* query= g_strdup_printf("SHOW CREATE DATABASE `%s`", database);
	if (mysql_query(conn, query)) {
		g_free(query);
                query= g_strdup_printf("CREATE DATABASE `%s`", database);
                mysql_query(conn, query);
	} else {
		// Need to clear the query
		MYSQL_RES *result= mysql_store_result(conn);
		mysql_free_result(result);
	}
	g_free(query);

	if (overwrite_tables) {
		query= g_strdup_printf("DROP TABLE IF EXISTS `%s`.`%s`", database, table);
		mysql_query(conn, query);
		g_free(query);
	}

	restore_data(conn, database, table, filename);
	g_strfreev(split_table);
	g_strfreev(split_file);
	return;
}

void add_table(const gchar* filename, struct configuration *conf) {
	struct job *j= g_new0(struct job, 1);
	struct restore_job *rj= g_new(struct restore_job, 1);
	j->job_data= (void*) rj;
	rj->filename= g_strdup(filename);
	j->type= JOB_RESTORE;
	gchar** split_file= g_strsplit(filename, ".", 0);
	rj->database= g_strdup(split_file[0]);
	rj->table= g_strdup(split_file[1]);
	g_async_queue_push(conf->queue, j);
	return;
}

void *process_queue(struct configuration *conf) {
	g_mutex_lock(init_mutex);
	MYSQL *thrconn= mysql_init(NULL);
	g_mutex_unlock(init_mutex);

	mysql_options(thrconn, MYSQL_READ_DEFAULT_GROUP, "myloader");

	if (compress_protocol)
		mysql_options(thrconn, MYSQL_OPT_COMPRESS, NULL);

	if (!mysql_real_connect(thrconn, hostname, username, password, NULL, port, socket_path, 0)) {
		g_critical("Failed to connect to MySQL server: %s", mysql_error(thrconn));
		exit(EXIT_FAILURE);
	}

	mysql_query(thrconn, "/*!40101 SET NAMES binary*/");
	mysql_query(thrconn, "SET autocommit=0");

	g_async_queue_push(conf->ready, GINT_TO_POINTER(1));

	struct job* job= NULL;
	struct restore_job* rj= NULL;
	for(;;) {
		job= (struct job*)g_async_queue_pop(conf->queue);

		switch (job->type) {
			case JOB_RESTORE:
				rj= (struct restore_job *)job->job_data;
				restore_data(thrconn, rj->database, rj->table, rj->filename);
				if (rj->database) g_free(rj->database);
                                if (rj->table) g_free(rj->table);
                                if (rj->filename) g_free(rj->filename);
                                g_free(rj);
                                g_free(job);
                                break;
			case JOB_SHUTDOWN:
				if (thrconn)
					mysql_close(thrconn);
				g_free(job);
				mysql_thread_end();
				return NULL;
				break;
			default:
				g_critical("Something very bad happened!");
				exit(EXIT_FAILURE);
		}
	}
	if (thrconn)
		mysql_close(thrconn);
	mysql_thread_end();
	return NULL;
}

void restore_data(MYSQL *conn, char *database, char *table, const char *filename) {
	void *infile;
	gboolean is_compressed= FALSE;
	gboolean eof= FALSE;
	guint query_counter= 0;
	GString *data= g_string_sized_new(512);

	gchar* path= g_build_filename(directory, filename, NULL);

	if (!g_str_has_suffix(path, ".gz")) {
		infile= g_fopen(path, "r");
		is_compressed= FALSE;
	} else {
		infile= gzopen(path, "r");
		is_compressed= TRUE;
	}

	if (!infile) {
		g_critical("cannot open file %s (%d)", filename, errno);
		errors++;
		return;
	}

	gchar *query= g_strdup_printf("USE `%s`", database);
	if (mysql_query(conn, query)) {
		g_critical("Error switching to database %s whilst restoring table %s", database, table);
		g_free(query);
		errors++;
		return;
	}

	g_free(query);

	mysql_query(conn, "START TRANSACTION");

	while (eof == FALSE) {
		if (read_data(infile, is_compressed, data, &eof)) {
			// Search for ; in last 5 chars of line
			if (g_strrstr(&data->str[data->len-5], ";\n")) { 
				if (mysql_real_query(conn, data->str, data->len)) {
					g_critical("Error restoring %s.%s: %s", database, table, mysql_error(conn));
					errors++;
					return;
				}
				query_counter++;
				if (query_counter == commit_count) {
					query_counter= 0;
					if (mysql_query(conn, "COMMIT")) {
						g_critical("Error commiting data for %s.%s: %s", database, table, mysql_error(conn));
						errors++;
						return;
					}
					mysql_query(conn, "START TRANSACTION");
				}

				g_string_set_size(data, 0);
			}
		} else {
			g_critical("error reading file %s (%d)", filename, errno);
			errors++;
			return;
		}
	}
	if (mysql_query(conn, "COMMIT")) {
		g_critical("Error commiting data for %s.%s: %s", database, table, mysql_error(conn));
		errors++;
	}

	g_free(path);
	return;
}

gboolean read_data(FILE *file, gboolean is_compressed, GString *data, gboolean *eof) {
	char buffer[256];

	do {
		if (!is_compressed) {
			if (fgets(buffer, 256, file) == NULL) {
				if (feof(file)) {
					*eof= TRUE;
				} else {
					return FALSE;
				}
			}
		} else {
			if (!gzgets((gzFile)file, buffer, 256)) {
				if (gzeof(file)) {
					*eof= TRUE;
				} else {
					return FALSE;
				}
			}
		}
		g_string_append(data, buffer);
	} while ((buffer[strlen(buffer)] != '\0') && *eof == FALSE);

	return TRUE;
}
