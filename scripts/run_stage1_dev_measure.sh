cd /h/DMU/Contest/hunik/object-detector
export MSYS2_ARG_CONV_EXCL="*"
EXE='C:\Users\Yesung\AppData\Local\Temp\hunik-full\Release\unmanned_detector.exe'
for c in 13 10 12 09 11 08; do
  echo "== pair $c"; python -I scripts/measure_pair_features.py --exe "$EXE" --model models/yolo11n-pose-416x224.onnx --index 'E:\hunik-anomaly\anomaly_index.csv' --work 'C:\Users\Yesung\AppData\Local\hunik-anom\pair' --cache-dir 'E:\hunik-anomaly\_work\cache_dev_416x224' --split dev --pos-class $c --neg-per-class 20 > /c/Users/Yesung/AppData/Local/hunik-anom/pair_$c.out 2>&1
  echo "== pixel $c"; python -I scripts/measure_anomaly_filter_auc.py --exe "$EXE" --model models/yolo11n-pose-416x224.onnx --index 'E:\hunik-anomaly\anomaly_index.csv' --work 'C:\Users\Yesung\AppData\Local\hunik-anom\pix' --cache-dir 'E:\hunik-anomaly\_work\cache_dev_416x224' --split dev --pos-class $c --neg-classes $(echo 07,08,09,10,11,12,13,14 | sed "s/\b$c\b//;s/,,/,/;s/^,//;s/,$//") --neg-per-class 12 --cached-only > /c/Users/Yesung/AppData/Local/hunik-anom/pix_$c.out 2>&1
done
echo ALLDONE
