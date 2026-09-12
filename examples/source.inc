; A pure cosine carrier, sampled at the orchestra rate.
; Prepared at i-time for offline scores. Each grain applies its own envelope.
iSource ftgen 0, 0, -96000, -2, 0
iIndex = 0
while iIndex < 96000 do
  tableiw cos(2*$M_PI*220*iIndex/sr), iIndex, iSource
  iIndex += 1
od
