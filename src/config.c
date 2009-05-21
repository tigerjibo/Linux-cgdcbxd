/*
 * Copyright IBM Corporation. 2007
 *
 * Authors:	Balbir Singh <balbir@linux.vnet.ibm.com>
 *		Dhaval Giani <dhaval@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * TODOs:
 * 	1. Implement our own hashing scheme
 * 	2. Add namespace support
 * 	3. Add support for parsing cgroup filesystem and creating a
 * 	   config out of it.
 *
 * Code initiated and designed by Balbir Singh. All faults are most likely
 * his mistake.
 *
 * Cleanup and changes to use the "official" structures and functions made
 * by Dhaval Giani. All faults will still be Balbir's mistake :)
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <libcgroup.h>
#include <libcgroup-internal.h>
#include <limits.h>
#include <pwd.h>
#include <pthread.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_CGROUPS 1024

extern FILE *yyin;
extern int yyparse(void);

/*
 * The basic global data structures.
 *
 * config_mount_table -> Where what controller is mounted
 * table_index -> Where in the table are we.
 * config_table_lock -> Serializing access to config_mount_table.
 * cgroup_table -> Which cgroups have to be created.
 * cgroup_table_index -> Where in the cgroup_table we are.
 */
static struct cg_mount_table_s config_mount_table[CG_CONTROLLER_MAX];
static int config_table_index;
static pthread_rwlock_t config_table_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct cgroup config_cgroup_table[MAX_CGROUPS];
int cgroup_table_index;

/*
 * Needed for the type while mounting cgroupfs.
 */
static const char cgroup_filesystem[] = "cgroup";

/*
 * NOTE: All these functions return 1 on success
 * and not 0 as is the library convention
 */

/*
 * This call just sets the name of the cgroup. It will
 * always be called in the end, because the parser will
 * work bottom up.
 */
int cgroup_config_insert_cgroup(char *cg_name)
{
	struct cgroup *config_cgroup =
			&config_cgroup_table[cgroup_table_index];

	strncpy(config_cgroup->name, cg_name, FILENAME_MAX);
	/*
	 * Since this will be the last part to be parsed.
	 */
	cgroup_table_index++;
	free(cg_name);
	return 1;
}

/*
 * This function sets the various controller's control
 * files. It will always append values for cgroup_table_index
 * entry in the cgroup_table. The index is incremented in
 * cgroup_config_insert_cgroup
 */
int cgroup_config_parse_controller_options(char *controller, char *name_value)
{
	char *buffer = NULL;
	char *name, *value;
	struct cgroup_controller *cgc;
	int error;
	struct cgroup *config_cgroup =
		&config_cgroup_table[cgroup_table_index];
	char *nm_pairs, *nv_buf;

	cgroup_dbg("Adding controller %s, value %s\n", controller, name_value);
	cgc = cgroup_add_controller(config_cgroup, controller);

	if (!cgc)
		goto parse_error;

	/*
	 * Did we just specify the controller to create the correct
	 * set of directories, without setting any values?
	 */
	if (!name_value)
		goto done;

	nm_pairs = strtok_r(name_value, ":", &nv_buf);
	cgroup_dbg("[1] name value pair being processed is %s\n", nm_pairs);

	name = strtok_r(nm_pairs, " ", &buffer);

	if (!name)
		goto parse_error;

	value = strtok_r(NULL, " ", &buffer);

	if (!value)
		goto parse_error;

	cgroup_dbg("name is %s, value is %s\n", name, value);
	error = cgroup_add_value_string(cgc, name, value);

	if (error)
		goto parse_error;

	while ((nm_pairs = strtok_r(NULL, ":", &nv_buf))) {
		cgroup_dbg("[2] name value pair being processed is %s\n",
			nm_pairs);
		name = strtok_r(nm_pairs, " ", &buffer);

		if (!name)
			goto parse_error;

		value = strtok_r(NULL, " ", &buffer);

		if (!value)
			goto parse_error;

		cgroup_dbg("name is %s, value is %s\n", name, value);
		error = cgroup_add_value_string(cgc, name, value);

		if (error)
			goto parse_error;
	}

done:
	free(controller);
	free(name_value);
	return 1;

parse_error:
	free(controller);
	free(name_value);
	cgroup_delete_cgroup(config_cgroup, 1);
	cgroup_table_index--;
	return 0;
}

/*
 * Sets the tasks file's uid and gid
 */
int cgroup_config_group_task_perm(char *perm_type, char *value)
{
	struct passwd *pw, *pw_buffer;
	struct group *group, *group_buffer;
	int error;
	long val = atoi(value);
	char buffer[CGROUP_BUFFER_LEN];
	struct cgroup *config_cgroup =
			&config_cgroup_table[cgroup_table_index];

	if (!strcmp(perm_type, "uid")) {
		if (!val) {
			pw = (struct passwd *) malloc(sizeof(struct passwd));

			if (!pw)
				goto group_task_error;

			error = getpwnam_r(value, pw, buffer, CGROUP_BUFFER_LEN,
								&pw_buffer);
			if (pw_buffer == NULL) {
				free(pw);
				goto group_task_error;
			}

			val = pw->pw_uid;
			free(pw);
		}
		config_cgroup->tasks_uid = val;
	}

	if (!strcmp(perm_type, "gid")) {
		if (!val) {
			group = (struct group *) malloc(sizeof(struct group));

			if (!group)
				goto group_task_error;

			error = getgrnam_r(value, group, buffer,
					CGROUP_BUFFER_LEN, &group_buffer);

			if (group_buffer == NULL) {
				free(group);
				goto group_task_error;
			}

			val = group->gr_gid;
			free(group);
		}
		config_cgroup->tasks_gid = val;
	}

	free(perm_type);
	free(value);
	return 1;

group_task_error:
	free(perm_type);
	free(value);
	cgroup_delete_cgroup(config_cgroup, 1);
	cgroup_table_index--;
	return 0;
}

/*
 * Set the control file's uid/gid
 */
int cgroup_config_group_admin_perm(char *perm_type, char *value)
{
	struct passwd *pw, *pw_buffer;
	struct group *group, *group_buffer;
	int error;
	struct cgroup *config_cgroup =
				&config_cgroup_table[cgroup_table_index];
	long val = atoi(value);
	char buffer[CGROUP_BUFFER_LEN];

	if (!strcmp(perm_type, "uid")) {
		if (!val) {
			pw = (struct passwd *) malloc(sizeof(struct passwd));

			if (!pw)
				goto admin_error;

			error = getpwnam_r(value, pw, buffer, CGROUP_BUFFER_LEN,
								&pw_buffer);
			if (pw_buffer == NULL) {
				free(pw);
				goto admin_error;
			}

			val = pw->pw_uid;
			free(pw);
		}
		config_cgroup->control_uid = val;
	}

	if (!strcmp(perm_type, "gid")) {
		if (!val) {
			group = (struct group *) malloc(sizeof(struct group));

			if (!group)
				goto admin_error;

			error = getgrnam_r(value, group, buffer,
					CGROUP_BUFFER_LEN, &group_buffer);

			if (group_buffer == NULL) {
				free(group);
				goto admin_error;
			}

			val = group->gr_gid;
			free(group);
		}
		config_cgroup->control_gid = val;
	}

	free(perm_type);
	free(value);
	return 1;

admin_error:
	free(perm_type);
	free(value);
	cgroup_delete_cgroup(config_cgroup, 1);
	cgroup_table_index--;
	return 0;
}

/*
 * The moment we have found the controller's information
 * insert it into the config_mount_table.
 */
int cgroup_config_insert_into_mount_table(char *name, char *mount_point)
{
	int i;

	if (config_table_index >= CG_CONTROLLER_MAX)
		return 0;

	pthread_rwlock_wrlock(&config_table_lock);

	/*
	 * Merge controller names with the same mount point
	 */
	for (i = 0; i < config_table_index; i++) {
		if (strcmp(config_mount_table[i].path, mount_point) == 0) {
			char *cname = config_mount_table[i].name;
			strncat(cname, ",", FILENAME_MAX - strlen(cname) - 1);
			strncat(cname, name,
					FILENAME_MAX - strlen(cname) - 1);
			goto done;
		}
	}

	strcpy(config_mount_table[config_table_index].name, name);
	strcpy(config_mount_table[config_table_index].path, mount_point);
	config_table_index++;
done:
	pthread_rwlock_unlock(&config_table_lock);
	free(name);
	free(mount_point);
	return 1;
}

/*
 * Cleanup all the data from the config_mount_table
 */
void cgroup_config_cleanup_mount_table(void)
{
	memset(&config_mount_table, 0,
			sizeof(struct cg_mount_table_s) * CG_CONTROLLER_MAX);
}

/*
 * Start mounting the mount table.
 */
int cgroup_config_mount_fs()
{
	int ret;
	struct stat buff;
	int i;

	for (i = 0; i < config_table_index; i++) {
		struct cg_mount_table_s *curr =	&(config_mount_table[i]);

		ret = stat(curr->path, &buff);
		
		if (ret < 0 && errno != ENOENT) {
			last_errno = errno;
			return ECGOTHER;
		}

		if (errno == ENOENT) {
			ret = mkdir(curr->path,
					S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
			if (ret < 0) {
				last_errno = errno;
				return ECGOTHER;
			}
		} else if (!S_ISDIR(buff.st_mode)) {
			errno = ENOTDIR;
			last_errno = errno;
			return ECGOTHER;
		}

		ret = mount(cgroup_filesystem, curr->path, cgroup_filesystem,
								0, curr->name);

		if (ret < 0)
			return ECGMOUNTFAIL;
	}
	return 0;
}

/*
 * Actually create the groups once the parsing has been finished.
 */
int cgroup_config_create_groups()
{
	int error = 0;
	int i;

	for (i = 0; i < cgroup_table_index; i++) {
		struct cgroup *cgroup = &config_cgroup_table[i];
		error = cgroup_create_cgroup(cgroup, 0);
		cgroup_dbg("creating group %s, error %d\n", cgroup->name,
			error);
		if (error)
			return error;
	}
	return error;
}

/*
 * Destroy the cgroups
 */
int cgroup_config_destroy_groups(void)
{
	int error = 0;
	int i;

	for (i = 0; i < cgroup_table_index; i++) {
		struct cgroup *cgroup = &config_cgroup_table[i];
		error = cgroup_delete_cgroup(cgroup, 0);
		if (error)
			return error;
	}
	return error;
}

/*
 * Unmount the controllers
 */
int cgroup_config_unmount_controllers(void)
{
	int i;
	int error;

	for (i = 0; i < config_table_index; i++) {
		/*
		 * We ignore failures and ensure that all mounted
		 * containers are unmounted
		 */
		error = umount(config_mount_table[i].path);
		if (error < 0)
			cgroup_dbg("Unmount failed\n");
		error = rmdir(config_mount_table[i].path);
		if (error < 0)
			cgroup_dbg("rmdir failed\n");
	}

	return 0;
}

/*
 * The main function which does all the setup of the data structures
 * and finally creates the cgroups
 */
int cgroup_config_load_config(const char *pathname)
{
	int error;
	yyin = fopen(pathname, "r");

	if (!yyin) {
		cgroup_dbg("Failed to open file %s\n", pathname);
		last_errno = errno;
		return ECGOTHER;
	}

	if (yyparse() != 0) {
		cgroup_dbg("Failed to parse file %s\n", pathname);
		fclose(yyin);
		return ECGROUPPARSEFAIL;
	}

	error = cgroup_config_mount_fs();
	if (error)
		goto err_mnt;

	error = cgroup_init();
	if (error)
		goto err_mnt;

	error = cgroup_config_create_groups();
	cgroup_dbg("creating all cgroups now, error=%d\n", error);
	if (error)
		goto err_grp;

	fclose(yyin);
	return 0;
err_grp:
	cgroup_config_destroy_groups();
err_mnt:
	cgroup_config_unmount_controllers();
	fclose(yyin);
	return error;
}