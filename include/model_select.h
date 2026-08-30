#ifndef MODEL_SELECT_H
#define MODEL_SELECT_H

#include <stddef.h>

/*
 * model_dir 안의 *-WIDTHxHEIGHT.onnx 파일 중 cam_w×cam_h 카메라에
 * letterbox 낭비가 가장 적은 파일을 골라 out_path에 경로를 씁니다.
 *
 * 파일명 규칙: 끝에 -WxH.onnx 가 있어야 합니다.
 * 예) yolo11n-416x224.onnx, yolo11n-416x288.onnx
 *
 * 반환: 1 = 선택됨, 0 = 후보 없음(폴더가 비어 있거나 패턴 불일치)
 */
int model_select(const char *model_dir, int cam_w, int cam_h,
                 char *out_path, size_t out_size);

#endif /* MODEL_SELECT_H */
