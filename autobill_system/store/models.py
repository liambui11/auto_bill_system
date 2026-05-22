from django.db import models

class Invoice(models.Model):
    STATUS_CHOICES = [('OPEN', 'Open'), ('CLOSED', 'Closed'), ('LOCKED', 'Locked (Needs Support)')]
    status = models.CharField(max_length=10, choices=STATUS_CHOICES, default='OPEN')
    rescan_count = models.IntegerField(default=0)
    created_at = models.DateTimeField(auto_now_add=True)

class InvoiceItem(models.Model):
    invoice = models.ForeignKey(Invoice, related_name='items', on_delete=models.CASCADE)
    product_name = models.CharField(max_length=255)
    weight = models.FloatField()
    price = models.FloatField()
    image = models.ImageField(upload_to='captured_images/', null=True, blank=True)
    created_at = models.DateTimeField(auto_now_add=True)