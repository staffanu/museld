function unused=printMatrixAsCode(m)
  for i = 1:size(m)(1)
    printf("IndexedSeq(")
    for j = 1:size(m)(2)
      printf("%f", m(i,j))
      if j < size(m)(2) printf(", ") endif
    endfor
    printf("),\n")
  endfor

  printf("\n");

  for i = 1:size(m)(1)
    for j = 1:size(m)(2)
      printf("%f", m(i,j))
      if j < size(m)(2) printf(", ") endif
    endfor
    printf(",\n")
  endfor

  endfunction

