import cv2
import numpy as np
import os
from django.conf import settings
from django.core.files.base import ContentFile
from django.http import JsonResponse, StreamingHttpResponse
from django.shortcuts import render, get_object_or_404, redirect
from django.views.decorators.csrf import csrf_exempt
from ultralytics import YOLO
from .models import Invoice, InvoiceItem

# ===== CHUẨN LOAD MODEL BẰNG BASE_DIR =====
model = None
model_path = os.path.join(settings.BASE_DIR, 'model', 'best.pt')

if os.path.exists(model_path):
    try:
        model = YOLO(model_path)
        print(f"✅ Model loaded successfully from {model_path}")
    except Exception as e:
        print("❌ Model load failed:", e)
else:
    print(f"❌ Model file not found at {model_path}")

PRICE_LIST = {
    'lemon': 35000, 
    'chili': 40000, 
    'banana': 25000, 
    'tomato': 30000, 
    'apple': 50000, 
    'grapes': 90000, 
    'raspberry': 120000, 
    'blackberries': 150000,
    'Unknown': 0
}

# --- API CHO HARDWARE ESP32 (Pull từ Stream, đã RESIZE) ---
@csrf_exempt
def predict_and_add(request):
    if request.method == 'POST' or request.method == 'GET':
        invoice = Invoice.objects.filter(status='OPEN').last()
        if not invoice:
            return JsonResponse({'error': 'No active session'}, status=403)

        weight = float(request.GET.get('weight', request.POST.get('weight', 0)))
        cam_ip = request.GET.get('cam_ip', request.POST.get('cam_ip', ''))

        if not cam_ip:
            return JsonResponse({'error': 'Missing camera IP'}, status=400)

        detected_class = "Unknown"
        stream_url = f"http://192.168.1.24:81/stream"

        try:
            cap = cv2.VideoCapture(stream_url)
            ret, frame = cap.read()
            cap.release()

            if ret and model:
                # ✅ RESIZE về 640x640 trước khi predict
                frame_resized = cv2.resize(frame, (640, 640))
                results = model.predict(frame_resized, conf=0.25, verbose=False)

                # VẼ bounding box
                plotted_frame = results[0].plot()
                _, buffer = cv2.imencode('.jpg', plotted_frame)

                if len(results[0].boxes) > 0:
                    class_id = int(results[0].boxes.cls[0].item())
                    detected_class = model.names[class_id]
                    if detected_class.endswith('-bag'):
                        detected_class = detected_class.replace('-bag', '')

                unit_price = PRICE_LIST.get(detected_class, 0)
                total_price = unit_price * weight

                item = InvoiceItem(
                    invoice=invoice,
                    product_name=detected_class,
                    weight=weight,
                    price=total_price
                )
                image_name = f"detect_{item.id}_{np.random.randint(1000)}.jpg"
                item.image.save(image_name, ContentFile(buffer.tobytes()), save=False)
                item.save()

                # ✅ Đã bỏ image_url khỏi response
                return JsonResponse({
                    'name': item.product_name,
                    'price': float(item.price),
                    'weight': float(item.weight)
                })
        except Exception as e:
            print(f"Stream Fetch Error: {e}")
            detected_class = "Error"

        return JsonResponse({'error': 'Failed to fetch frame or process'}, status=500)
    return JsonResponse({'error': 'Invalid request'}, status=400)

# --- LIVE STREAM (đã RESIZE) ---
def gen_frames(cam_ip):
    stream_url = f"http://192.168.1.24:81/stream"
    cap = cv2.VideoCapture(stream_url)
    while True:
        success, frame = cap.read()
        if not success:
            break
        else:
            if model:
                # ✅ RESIZE trước khi predict
                frame_resized = cv2.resize(frame, (640, 640))
                results = model.predict(frame_resized, conf=0.25, verbose=False)
                frame = results[0].plot()

            ret, buffer = cv2.imencode('.jpg', frame)
            frame = buffer.tobytes()
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')

def video_feed(request):
    cam_ip = request.GET.get('cam_ip', '')
    if not cam_ip:
        return StreamingHttpResponse("Missing cam_ip parameter", status=400)
    return StreamingHttpResponse(gen_frames(cam_ip),
                                 content_type='multipart/x-mixed-replace; boundary=frame')

def check_session_status(request):
    """API cho ESP32 kiểm tra trạng thái trước khi cân"""
    invoice = Invoice.objects.filter(status__in=['OPEN', 'LOCKED']).last()
    active = invoice is not None and invoice.status == 'OPEN'
    locked = invoice.status == 'LOCKED' if invoice else False
    return JsonResponse({'active': active, 'locked': locked})

# --- API CHO FRONTEND (Realtime Dashboard) ---
def api_get_cart(request):
    invoice = Invoice.objects.filter(status__in=['OPEN', 'LOCKED']).last()
    if not invoice:
        return JsonResponse({'has_session': False})

    items = []
    for item in invoice.items.all():
        # ✅ Đã bỏ trường 'image' khỏi response giỏ hàng
        items.append({
            'id': item.id,
            'product_name': item.product_name,
            'weight': item.weight,
            'price': item.price
        })

    total = sum(item['price'] for item in items)

    return JsonResponse({
        'has_session': True,
        'status': invoice.status,
        'rescan_count': invoice.rescan_count,
        'items': items,
        'total_amount': total
    })

# --- WEB CONTROLLERS ---
def dashboard(request):
    invoice = Invoice.objects.filter(status__in=['OPEN', 'LOCKED']).last()
    return render(request, 'dashboard.html', {'invoice': invoice})

def start_invoice(request):
    Invoice.objects.filter(status__in=['OPEN', 'LOCKED']).update(status='CLOSED')
    Invoice.objects.create(status='OPEN')
    return redirect('dashboard')

def delete_item(request, item_id):
    item = get_object_or_404(InvoiceItem, id=item_id)
    item.delete()
    return redirect('dashboard')

def rescan_item(request, item_id):
    item = get_object_or_404(InvoiceItem, id=item_id)
    invoice = item.invoice
    item.delete()
    invoice.rescan_count += 1
    if invoice.rescan_count >= 3:
        invoice.status = 'LOCKED'
    invoice.save()
    return redirect('dashboard')

def confirm_invoice(request, invoice_id):
    invoice = get_object_or_404(Invoice, id=invoice_id)
    invoice.status = 'CLOSED'
    invoice.save()
    return redirect('dashboard')

# --- API TEST: UPLOAD ẢNH (đã RESIZE) ---
@csrf_exempt
def test_predict_upload(request):
    if request.method == 'POST':
        invoice = Invoice.objects.filter(status='OPEN').last()
        if not invoice:
            return JsonResponse({'error': 'No active session. Please click "Start Invoice" first.'}, status=403)

        weight = float(request.POST.get('weight', 0.5))
        img_file = request.FILES.get('image')

        if not img_file:
            return JsonResponse({'error': 'No image file uploaded'}, status=400)

        try:
            file_bytes = np.asarray(bytearray(img_file.read()), dtype=np.uint8)
            frame = cv2.imdecode(file_bytes, cv2.IMREAD_COLOR)

            if frame is None:
                return JsonResponse({'error': 'Invalid image file'}, status=400)

            if model:
                # ✅ RESIZE về 640x640 trước khi predict
                frame_resized = cv2.resize(frame, (640, 640))
                results = model.predict(frame_resized, conf=0.25, verbose=False)

                plotted_frame = results[0].plot()
                _, buffer = cv2.imencode('.jpg', plotted_frame)

                detected_class = "Unknown"
                print("Boxes:", results[0].boxes)
                if len(results[0].boxes) > 0:
                    class_id = int(results[0].boxes.cls[0].item())
                    detected_class = model.names[class_id]
                    print("Detected raw:", detected_class)
                    if detected_class.endswith('-bag'):
                        detected_class = detected_class.replace('-bag', '')

                unit_price = PRICE_LIST.get(detected_class, 0)
                total_price = unit_price * weight

                item = InvoiceItem(
                    invoice=invoice,
                    product_name=detected_class,
                    weight=weight,
                    price=total_price
                )
                image_name = f"test_{np.random.randint(1000)}.jpg"
                item.image.save(image_name, ContentFile(buffer.tobytes()), save=False)
                item.save()

                # ✅ Đã bỏ image_result_url khỏi response
                return JsonResponse({
                    'status': 'success',
                    'detected': detected_class,
                    'weight': weight,
                    'price': total_price
                })
            else:
                return JsonResponse({'error': 'Model not loaded'}, status=500)
        except Exception as e:
            return JsonResponse({'error': str(e)}, status=500)

    return render(request, 'test_upload.html')