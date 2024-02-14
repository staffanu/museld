function misfit = eval_efm_fir_filter(filter_taps, data, decimation, do_plot);

filter_taps=filter_taps / sum(filter_taps);
longefm = conv(filter_taps, data);

longefm(1:2)=1; % make sure no sign flip at first index
longefm(end)=1; % make sure no sign flip at first index
zci = find(longefm.*circshift(longefm, 1) <= 0);
zc = zci-1 + longefm(zci-1)./(longefm(zci-1)-longefm(zci));
distances = zc(2:end)-zc(1:end-1);

distances(distances > 200 / decimation) = 200 / decimation;

Fs=62.5e6 / decimation;
ref_dists = [3:11] * Fs / (44.1e3 * 32 * 588/192);

multirow_dists=[repmat(ref_dists', [1  length(distances)])];
multirow_refdists = repmat(distances, [length(ref_dists) 1]);
[a,b]=meshgrid(ones(length(distances), 1), (ref_dists));
closest = sum((min(abs(multirow_dists-multirow_refdists)) == abs(multirow_dists-multirow_refdists)) .* b);

misfit = (sum((distances - closest).^2) / length(distances))^(1/2);

if do_plot
  figure(1);
  plot(longefm(1:500));

  figure(2)
  hist(distances, 500)
endif

endfunction;

