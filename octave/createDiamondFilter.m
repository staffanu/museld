function filter = createDiamondFilter(nx, ny, xFactor, yFactor)

[X, Y] = meshgrid([-1:2 / (nx-1):1], [-1:2/ (ny-1):1]);

response = @(x) (x < 0.9) * 1.0 + (x >= 0.9 & x < 1.1) .* (0.5 + 1 - x);

diamond = response(abs(X * xFactor) + abs(Y * yFactor))

rolloff = hamming(ny) * hamming(nx)'
cfilter = fftshift(ifft2(ifftshift(diamond)));
rolledoff = real(cfilter) .* rolloff;
filter = rolledoff / sum(sum(rolledoff))

endfunction
