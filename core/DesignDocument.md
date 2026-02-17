1. To handle exception where BlindModeSwitchOfPacked can have Exception MODE new LOCALITY->ST_EXCEPTION_BIT_FAULTY::Introduced.
2. 
|15      12|11 9|8|7   4|3 2|1 0|
|Priority   |Loc |T|RelM |RO |DT |

3. RelOffset(2bit):: Unsigned
        RelOffset semantics you 
        1 indicates “head pointing to tail” (head → tail).
        2 indicates “tail pointing to head”.
        0 indicates standalone (no pointer pairing).
        Consumers should treat only reloff == +1 or -1 as paired; anything else is standalone or ignored. -2 -> is reserved for now.