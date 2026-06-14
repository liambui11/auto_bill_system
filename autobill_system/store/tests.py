from django.test import TestCase
from django.urls import reverse
from .models import Invoice

class DashboardPricelistTestCase(TestCase):
    def test_dashboard_without_active_invoice_contains_products(self):
        """Kiểm tra màn hình dashboard khi chưa bắt đầu hóa đơn phải chứa danh sách sản phẩm đã định dạng"""
        response = self.client.get(reverse('dashboard'))
        self.assertEqual(response.status_code, 200)
        self.assertIn('products', response.context)
        
        products = response.context['products']
        self.assertTrue(len(products) > 0)
        
        # Đảm bảo không chứa sản phẩm 'Unknown'
        product_names = [p['name'] for p in products]
        self.assertNotIn('Unknown', product_names)
        self.assertNotIn('unknown', product_names)
        
        # Đảm bảo tên sản phẩm có dấu gạch dưới đã được thay thế/dịch sang tiếng Việt
        self.assertIn('Ớt chuông', product_names)
        self.assertNotIn('bell_pepper', product_names)
        
        # Kiểm tra cấu trúc phần tử đầu tiên
        first_product = products[0]
        self.assertIn('name', first_product)
        self.assertIn('price', first_product)
        self.assertIn('formatted_price', first_product)
        
        # Kiểm tra sự tồn tại và định dạng giá của sản phẩm 'Táo' (phân tách hàng nghìn bằng dấu chấm)
        apple_product = next((p for p in products if p['name'] == 'Táo'), None)
        self.assertIsNotNone(apple_product, "Sản phẩm 'Táo' phải tồn tại trong bảng giá")
        self.assertEqual(apple_product['formatted_price'], '50.000')
                
    def test_dashboard_with_active_invoice_has_empty_products(self):
        """Kiểm tra khi có hóa đơn đang hoạt động (OPEN) thì danh sách products truyền sang phải rỗng"""
        invoice = Invoice.objects.create(status='OPEN')
        
        session = self.client.session
        session['active_invoice_id'] = invoice.id
        session.save()
        
        response = self.client.get(reverse('dashboard'))
        self.assertEqual(response.status_code, 200)
        self.assertIn('products', response.context)
        self.assertEqual(len(response.context['products']), 0)

    def test_dashboard_with_locked_invoice_has_empty_products(self):
        """Kiểm tra khi có hóa đơn bị khóa (LOCKED) thì danh sách products truyền sang phải rỗng"""
        invoice = Invoice.objects.create(status='LOCKED')
        
        session = self.client.session
        session['active_invoice_id'] = invoice.id
        session.save()
        
        response = self.client.get(reverse('dashboard'))
        self.assertEqual(response.status_code, 200)
        self.assertIn('products', response.context)
        self.assertEqual(len(response.context['products']), 0)
