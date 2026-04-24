// APC-native policy:
// dead -> can stay logically removed from registry, physical free happens by owner
// reclaim requested -> only clear reclaim bit after a grace-period boundary