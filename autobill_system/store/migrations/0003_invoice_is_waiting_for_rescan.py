# Generated manually

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('store', '0002_invoiceitem_image'),
    ]

    operations = [
        migrations.AddField(
            model_name='invoice',
            name='is_waiting_for_rescan',
            field=models.BooleanField(default=False),
        ),
    ]
