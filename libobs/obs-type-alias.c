/******************************************************************************
    Copyright (C) 2026 by Narta Xaymar Dirks <info@xaymar.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/* ID aliases for registered encoder and output types.
 *
 * An alias maps an old type ID to a new one. Aliases are resolved when an
 * object is created or a type is queried, never at registration, so the
 * target may be registered by another module, in any order. */

#include "obs-internal.h"

bool obs_type_alias_table_init(struct obs_type_alias_table *table)
{
	da_init(table->aliases);
	return true;
}

void obs_type_alias_table_free(struct obs_type_alias_table *table)
{
	for (size_t i = 0; i < table->aliases.num; i++) {
		bfree(table->aliases.array[i].old_id);
		bfree(table->aliases.array[i].new_id);
	}
	da_free(table->aliases);
}

/* Caller holds obs->registration_mutex. */
static struct obs_type_alias *find_type_alias(struct obs_type_alias_table *table, const char *old_id)
{
	for (size_t i = 0; i < table->aliases.num; i++) {
		struct obs_type_alias *alias = &table->aliases.array[i];
		if (strcmp(alias->old_id, old_id) == 0)
			return alias;
	}
	return NULL;
}

bool obs_type_alias_register(struct obs_type_alias_table *table, const char *kind, const char *old_id,
			     const char *new_id, obs_type_alias_migrate_t migrate, void *param,
			     const void *(*lookup)(const char *id))
{
	if (!old_id || !*old_id || !new_id || !*new_id || strcmp(old_id, new_id) == 0) {
		blog(LOG_WARNING, "obs_register_%s_alias: invalid arguments", kind);
		return false;
	}

	/* One lock with type registration, so a type registered by another
	 * module loading in parallel can't slip in between check and insert. */
	pthread_mutex_lock(&obs->registration_mutex);
	if (lookup(old_id)) {
		pthread_mutex_unlock(&obs->registration_mutex);
		blog(LOG_WARNING, "obs_register_%s_alias: a %s type named '%s' is already registered", kind, kind,
		     old_id);
		return false;
	}

	bool added = false;
	if (!find_type_alias(table, old_id)) {
		struct obs_type_alias *alias = da_push_back_new(table->aliases);
		alias->old_id = bstrdup(old_id);
		alias->new_id = bstrdup(new_id);
		alias->migrate = migrate;
		alias->param = param;
		added = true;
	}
	pthread_mutex_unlock(&obs->registration_mutex);

	if (!added)
		blog(LOG_WARNING, "obs_register_%s_alias: '%s' is already an alias", kind, old_id);
	return added;
}

const void *obs_type_alias_resolve(struct obs_type_alias_table *table, const char *kind, const char *id,
				   const void *(*lookup)(const char *id), struct obs_type_alias_hop *hops,
				   size_t *num_hops)
{
	*num_hops = 0;
	if (!id)
		return NULL;

	pthread_mutex_lock(&obs->registration_mutex);
	const void *info = lookup(id);
	if (info) {
		pthread_mutex_unlock(&obs->registration_mutex);
		return info;
	}

	const char *cur = id;
	for (size_t i = 0; i < OBS_TYPE_ALIAS_MAX_HOPS; i++) {
		struct obs_type_alias *alias = find_type_alias(table, cur);
		if (!alias)
			break;

		hops[i].old_id = alias->old_id;
		hops[i].migrate = alias->migrate;
		hops[i].param = alias->param;
		*num_hops = i + 1;

		info = lookup(alias->new_id);
		if (info)
			break;
		cur = alias->new_id;
	}
	pthread_mutex_unlock(&obs->registration_mutex);

	if (!info && *num_hops)
		blog(LOG_WARNING, "%s ID '%s' is an alias, but its target was not found", kind, id);
	return info;
}

obs_data_t *obs_type_alias_migrate(obs_data_t *settings, const struct obs_type_alias_hop *hops, size_t num_hops)
{
	if (!num_hops)
		return NULL;

	obs_data_t *migrated = obs_data_create();
	if (settings)
		obs_data_apply(migrated, settings);

	for (size_t i = 0; i < num_hops; i++) {
		if (hops[i].migrate)
			hops[i].migrate(hops[i].param, hops[i].old_id, migrated);
	}
	return migrated;
}
