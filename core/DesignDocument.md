// APC-native policy:
// dead -> can stay logically removed from registry, physical free happens by owner
// reclaim requested -> only clear reclaim bit after a grace-period boundary

OCCUPANCY_SNAPSHOT_OF_PUBLISHED_CELLS = total non-idle payload cells in this physical segment.
REGION_OCCUPANCY_X = published consumable cells physically inside X bounds and carrying relmask X.
PAGED_NODE_READY_BIT = exact bitmap of regions whose REGION_OCCUPANCY_X > 0.
FREE occupancy is not a counter. Free = layout_span - non_idle_inside_bounds.

IDEA:
Get rid of all and every linier scaning look for the best scanning algorithm to adapt for APC