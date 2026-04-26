// APC-native policy:
// dead -> can stay logically removed from registry, physical free happens by owner
// reclaim requested -> only clear reclaim bit after a grace-period boundary

IDEA:
fix and make current paired pointer mechanism  faster and safest and directly store a paired pointer of CompleteAPCNodeRegionsLayout in Header so that except construction or deconstruction as long as both half indicate published it safe to read and when claimed it safe to try to read that means any thread can access the same CompleteAPCNodeRegionsLayout even though they dont have the ownership that will keep track of proper occupancy and proper layout.