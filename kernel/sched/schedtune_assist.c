#include <linux/delay.h>
#include "sched.h"
#include "tune.h"

void assist_init(void)
{
	pr_info("assist init");
	set_stune_value("top-app", 5);
	set_stune_value("foreground", -5);
	set_stune_value("rt", 1);
	set_stune_value("background", -100);
};
void assist_remove(void)
{
	pr_info("assist reset");
	reset_stune_boost("top-app");
	reset_stune_boost("foreground");
	reset_stune_boost("rt");
	reset_stune_boost("background");
};
