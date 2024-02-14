function hpulse=rrc(Fs, Rs, r)
% needs pkg load signal
% Adapted from https://dsp.stackexchange.com/questions/68393/analog-square-root-raised-cosine-filter
%
% Fs: sample rate
% Rs: symbol rate
% r: bandwidth factor

sps = Fs/Rs; % samples per symbol

ntaps = 5 * sps + 1; % Pulse duration is 8 symbols

st = [-floor(ntaps/2):floor(ntaps/2)] / sps; % symbol time = t/Ts values
hpulse = 1/sqrt(sps) * (sin ((1-r)*pi*st) + 4*r*st.*cos((1+r)*pi*st)) ./ (pi*st.*(1-(4*r*st).^2));

% fix the removable singularities
hpulse(ceil(ntaps/2)) = 1/sqrt(sps) * (1 - r + 4*r/pi); % t = 0 singulatiry
sing_idx = find(abs(1-(4*r*st).^2) < 0.000001);
for k = [1:length(sing_idx)]
    hpulse(sing_idx) = 1/sqrt(sps) * r/sqrt(2) * ((1+2/pi)*sin(pi/(4*r))+(1-2/pi)*cos(pi/(4*r)));
endfor

% normalize to 0 dB gain
hpulse = hpulse / sum(hpulse);

