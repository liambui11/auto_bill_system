from django.urls import path
from . import views

urlpatterns = [
    path('', views.dashboard, name='dashboard'),
    path('start/', views.start_invoice, name='start_invoice'),
    path('delete/<int:item_id>/', views.delete_item, name='delete_item'),
    path('rescan/<int:item_id>/', views.rescan_item, name='rescan_item'),
    path('confirm/<int:invoice_id>/', views.confirm_invoice, name='confirm_invoice'),
    
    # API phần cứng (Khớp tuyệt đối với code ESP32)
    path('predict', views.predict_and_add, name='api_predict'),
    path('session/status', views.check_session_status, name='api_session_status'),
    
    # API Web Dashboard
    path('api/cart/data/', views.api_get_cart, name='api_get_cart'),
    path('video_feed/', views.video_feed, name='video_feed'),
    path('test/upload/', views.test_predict_upload, name='test_upload'),
]