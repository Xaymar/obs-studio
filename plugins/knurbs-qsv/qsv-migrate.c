#include "qsv-migrate.h"

void qsv_migrate_v1_settings(obs_data_t *settings)
{
	if (!settings)
		return;

	/* v1 ignored cqp, so a stray value must not survive: without a saved
	 * qpp, the old and new default (both 23) applies. */
	if (obs_data_has_user_value(settings, "qpp"))
		obs_data_set_int(settings, "cqp", obs_data_get_int(settings, "qpp"));
	else
		obs_data_erase(settings, "cqp");

	obs_data_erase(settings, "qpi");
	obs_data_erase(settings, "qpp");
	obs_data_erase(settings, "qpb");
	obs_data_erase(settings, "__ver");
}
