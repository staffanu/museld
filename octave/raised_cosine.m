function f=raised_cosine(beta, T, t)
  f = 1/T * sinc(t/T) .* cos(pi*beta*t/T) ./ (1 - (2*beta*t/T).^2);

  p = find(abs(abs(t) - T/(2*beta)) < 1e-10);
  f2 = pi/(4*T) * sinc(1/(2*beta));
  f(p) = f2;
endfunction

