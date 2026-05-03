// APC-native policy:
// dead -> can stay logically removed from registry, physical free happens by owner
// reclaim requested -> only clear reclaim bit after a grace-period boundary

IDEA:
Get rid of all and every linier scaning look for the best scanning algorithm to adapt for APC
Fix-> MakeInitialPacked() to also handle ->PackedCellNodeAuthority