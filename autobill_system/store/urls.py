from django.urls import path
from . import views

urlpatterns = [
    path('', views.dashboard, name='dashboard'),
    path('start/', views.start_invoice, name='start_invoice'),
    path('delete/<int:item_id>/', views.delete_item, name='delete_item'),
    path('rescan/<int:item_id>/', views.rescan_item, name='rescan_item'),
    path('reset-rescan/', views.reset_rescan_count, name='reset_rescan_count'),
    path('confirm/<int:invoice_id>/', views.confirm_invoice, name='confirm_invoice'),
    path('cancel/<int:invoice_id>/', views.cancel_invoice, name='cancel_invoice'),
    path('clear-last-paid/', views.clear_last_paid, name='clear_last_paid'),
    
    # API phần cứng (Khớp tuyệt đối với code ESP32)
    path('predict/', views.predict_and_add, name='api_predict'),
    path('session/status/', views.check_session_status, name='api_session_status'),
    
    # API Web Dashboard
    path('api/cart/data/', views.api_get_cart, name='api_get_cart'),
    path('api/history/', views.api_invoice_history, name='api_invoice_history'),
    path('video_feed/', views.video_feed, name='video_feed'),
    path('test/upload/', views.test_predict_upload, name='test_upload'),
    
    # Sandbox Test Images
    path('sandbox/', views.sandbox, name='sandbox'),
    path('sandbox/predict/', views.sandbox_predict, name='sandbox_predict'),
]