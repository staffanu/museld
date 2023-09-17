function retval = g2(w, z, Ts)
  retval = 1 + exp(-2 * z * w * Ts) ...
    - 2 * exp(-z * w * Ts) * cos(w * Ts * sqrt(1 - z^2));
endfunction

