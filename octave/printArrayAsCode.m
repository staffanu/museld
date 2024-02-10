function unused=printArrayAsCode(a)
  printf("[")
  for i = 1:length(a)
      printf("%f", a(i))
      if i < length(a) printf(", ") endif
  endfor
  printf("]\n")
endfunction

