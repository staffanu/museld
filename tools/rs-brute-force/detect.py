from scipy.spatial.distance import hamming

from rs import *

def detect_reedsolomon_parameters(message, mesecc_orig, gen_list=[2, 3, 5], c_exp=8):
    '''Use an exhaustive search to automatically find the correct parameters for the ReedSolomon codec from a sample message and its encoded RS code.
    Arguments: message is the sample message, eg, "hello world" ; mesecc_orig is the message variable encoded with RS block appended at the end.
    '''
    # Description: this is basically an exhaustive search where we will try every possible RS parameter, then try to encode the sample message, and see if the resulting RS code is close to the supplied code.
    # All variables except the Galois Field's exponent are automatically generated and searched.
    # To compare with the supplied RS code, we compute the Hamming distance, so that even if the RS code is tampered, we can still find the closest set of RS parameters to decode this message.

    # Init the variables
    n = len(mesecc_orig)
    k = len(message)
    field_charac = int((2**c_exp) - 1)
    maxval = max(mesecc_orig)
    if (max(mesecc_orig) > field_charac):
        raise ValueError("The specified field's exponent is wrong, the message contains values (%i) above the field's cardinality (%i)!" % (maxval, field_charac))

    # Prepare the variable that will store the result
    best_match = {"hscore": -1, "params": [{"gen_nb": 0, "prim": 0, "fcr": 0}]}

    # Exhaustively search by generating every combination of values for the RS parameters and test the Hamming distance
    for gen_nb in gen_list:
        prim_list = find_prime_polys(generator=gen_nb, c_exp=c_exp, fast_primes=False, single=False)
        for prim in prim_list:
            init_tables(prim, gen_nb, c_exp)
            for fcr in xrange(field_charac):
                #g = rs_generator_poly_all(n, fcr=fcr, generator=gen_nb)
                # Generate a RS code from the sample message using the current combination of RS parameters
                mesecc = rs_encode_msg(message, n-k, fcr, gen_nb)
                # Compute the Hamming distance
                h = int(hamming(mesecc, mesecc_orig) * len(mesecc) + 0.5)
                # If the Hamming distance is lower than the previous best match (or if it's the first try), save this set of parameters
                if best_match["hscore"] == -1 or h <= best_match["hscore"]:
                    # If the distance is strictly lower than for the previous match, then we replace the previous match with the current one
                    if best_match["hscore"] == -1 or h < best_match["hscore"]:
                        best_match["hscore"] = h
                        best_match["params"] = [{"gen_nb": gen_nb, "prim": prim, "fcr": fcr}]
                    # Else there is an ambiguity: the Hamming distance is the same as for the previous best match, so we keep the previous set of parameters but we append the current set
                    elif h == best_match["hscore"]:
                        best_match["params"].append({"gen_nb": gen_nb, "prim": prim, "fcr": fcr})
                    # If Hamming distance is 0, then we have found a perfect match (the current set of parameters allow to generate the exact same RS code from the sample message), so we stop here
                    if h == 0: break

    # Printing the results to the user
    if best_match["hscore"] >= 0:
        perfect_match_str = " (0=perfect match)" if best_match["hscore"]==0 else ""
        print("Found closest set of parameters, with Hamming distance %i%s:" % (best_match["hscore"], perfect_match_str))
        for param in best_match["params"]:
            print("gen_nb=%s prim=%s(%s) fcr=%s" % (param["gen_nb"], param["prim"], hex(param["prim"]), param["fcr"]))
    else:
        print("Parameters could not be automatically detected...")

