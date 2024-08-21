% The filter is constructed from the coefficients like this
% (example 7x7 filter):
% c1 c2 c3 c4 c3 c2 c1
% c2 c5 c6 c7 c6 c5 c2
% c3 c6 c8 c9
% c4 c7 c9  1
%
% An (n*2-1)x(n*2-1) filter thus requires n * (n+1) / 2 - 1 coefficients.

function F=vector_coeffs_to_filter(c)
  if size(c)(1) != 1
    c=c';
  endif
  n = sqrt((length(c)+1)*2+1/4)-1/2;
  if round(n) != n
    error "Incorrect number of coefficients"
  endif

  M=zeros(n);
  M(tril(ones(n,n))==1)=[c 1];
  M(triu(ones(n,n), 1)==1)=M'(triu(ones(n,n), 1)==1);

  F=[M M(:, n-1:-1:1);
     M(n-1:-1:1, :) M(n-1:-1:1, n-1:-1:1)];

  F = F / sum(sum(F));
endfunction

